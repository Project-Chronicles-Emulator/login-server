#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"
#include "data/starter_gear.hpp"
#include "persistence/postgres/character_rows.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#include <pqxx/pqxx>

namespace chronicles {

void validate_world_position(const WorldPosition &position) {
  if (position.map_id <= 0 || !std::isfinite(position.yaw)) {
    throw std::invalid_argument("World position has an invalid map or yaw");
  }
}

void validate_progress(int level, std::int64_t experience) {
  if (level < 1 || level > 1000 || experience < 0) {
    throw std::invalid_argument("Character progression is outside supported storage range");
  }
}

namespace {

constexpr int kAsciiUpperToLowerOffset = 32;

std::vector<CharacterItem> starter_items(int class_id) {
  std::vector<CharacterItem> items;
  const auto                 gear_items = starter_gear_for_class(class_id);

  items.reserve(gear_items.size());
  for (std::size_t index = 0; index < gear_items.size(); ++index) {
    const auto &  gear = gear_items[index];
    CharacterItem item;
    item.cms_id         = gear.cms_id;
    item.inventory_slot = static_cast<int>(index);
    item.equip_slot     = gear.slot_index;
    items.push_back(item);
  }

  return items;
}

void validate_shrine_revival(const WorldProgress &before, const WorldProgress &after,
                             const WorldPosition &position) {
  validate_world_position(position);
  if (!before.dead || after.dead) {
    throw std::invalid_argument("Shrine revival must transition a dead character to alive");
  }

  // General progress saves deliberately forbid life changes. Validate the same bounded
  // progress contract with life preserved; this dedicated transaction owns the revival.
  auto progress_after = after;
  progress_after.dead = before.dead;
  detail::validate_world_progress_transition(before, progress_after);
}

void validate_item(const CharacterItem &item) {
  if (item.cms_id <= 0 || item.inventory_slot < 0 || item.inventory_slot > 999 ||
      (item.equip_slot && (*item.equip_slot < 0 || *item.equip_slot >= kEquipSlotCount)) ||
      item.quantity <= 0 || item.durability < 0 || item.grade < 0 || item.grade > 255 ||
      item.polish_step < 0 || item.polish_step > kMaxItemPolishStep) {
    throw std::invalid_argument("Character item is outside supported storage range");
  }
}

bool valid_world_revision(std::int64_t revision) {
  return revision >= 0 && revision < std::numeric_limits<std::int64_t>::max();
}

bool valid_polish_change(std::int64_t expected_revision, const ItemPolishChange &change) {
  const bool known_outcome = change.outcome == PolishOutcome::success ||
                             change.outcome == PolishOutcome::fail ||
                             change.outcome == PolishOutcome::destruction;

  // The additive is a separate stack, so it can never be the tempering stone itself.
  const bool valid_additive =
      change.additive_cms_id >= 0 && change.additive_cms_id != change.material_cms_id;

  return valid_world_revision(expected_revision) && change.item_id > 0 && change.cms_id > 0 &&
         change.expected_polish_step >= 0 && change.expected_polish_step < kMaxItemPolishStep &&
         change.material_cms_id > 0 && change.material_count > 0 && change.gold_cost >= 0 &&
         valid_additive && known_outcome;
}

// Item statement for one tempering outcome. Parameters: $1 item id, $2 character id,
// $3 item CMS id, $4 expected polish step. Each statement returns a row only when the item
// still has the expected step, so a concurrent change rolls the whole attempt back.
const char *polish_outcome_statement(PolishOutcome outcome) {
  switch (outcome) {
  case PolishOutcome::success:
    return "UPDATE emulator_character_items SET polish_step=$4::integer+1 "
           "WHERE id=$1 AND character_id=$2 AND item_cms_id=$3 AND polish_step=$4 RETURNING id";
  case PolishOutcome::fail:
    // The step is unchanged; the no-op update only confirms the item and locks its row.
    return "UPDATE emulator_character_items SET polish_step=polish_step "
           "WHERE id=$1 AND character_id=$2 AND item_cms_id=$3 AND polish_step=$4 RETURNING id";
  case PolishOutcome::destruction:
    return "DELETE FROM emulator_character_items "
           "WHERE id=$1 AND character_id=$2 AND item_cms_id=$3 AND polish_step=$4 "
           "AND equip_slot IS NULL RETURNING id";
  }
  throw std::invalid_argument("Unknown tempering outcome");
}

bool valid_durability_changes(const std::vector<EquippedDurabilityChange> &changes) {
  std::set<std::int64_t> item_ids;
  std::set<int> equip_slots;

  for (const auto &change : changes) {
    if (change.item_id <= 0 || change.cms_id <= 0 || change.equip_slot < 0 ||
        change.equip_slot >= kEquipSlotCount || change.expected_durability < 0 ||
        change.durability < 0 || change.durability > change.expected_durability ||
        !item_ids.insert(change.item_id).second ||
        !equip_slots.insert(change.equip_slot).second) {
      return false;
    }
  }

  return true;
}

bool apply_item_updates(Character &character, const std::vector<CharacterItem> &items) {
  auto updated = character.items;
  std::set<std::int64_t> changed_ids;

  for (const auto &item : items) {
    if (!changed_ids.insert(item.id).second) {
      return false;
    }
    const auto found = std::ranges::find_if(updated, [&item](const CharacterItem &existing) {
      return existing.id == item.id && existing.cms_id == item.cms_id;
    });
    if (found == updated.end()) {
      return false;
    }
    *found = item;
  }

  // A swap must validate the fully updated inventory, not the intermediate slots.
  for (const auto &item : updated) {
    const bool slot_conflict = std::ranges::any_of(updated, [&](const auto &other) {
      return other.id != item.id &&
             (other.inventory_slot == item.inventory_slot ||
              (item.equip_slot && item.equip_slot == other.equip_slot));
    });
    if (slot_conflict) {
      return false;
    }
  }

  character.items = std::move(updated);
  return true;
}

bool valid_discard_request(std::int64_t expected_revision,
                           const std::vector<std::int64_t> &item_ids,
                           const std::map<int, int> &stackable_counts) {
  if (expected_revision < 0 ||
      expected_revision == std::numeric_limits<std::int64_t>::max() ||
      (item_ids.empty() && stackable_counts.empty())) {
    return false;
  }

  std::set<std::int64_t> distinct_ids;
  for (const auto item_id : item_ids) {
    if (item_id <= 0 || !distinct_ids.insert(item_id).second) {
      return false;
    }
  }
  for (const auto &[cms_id, count] : stackable_counts) {
    if (cms_id <= 0 || count <= 0) {
      return false;
    }
  }
  return true;
}

bool item_slots_available(const Character &character, const CharacterItem &item) {
  for (const auto &existing : character.items) {
    if (existing.id == item.id) {
      continue;
    }

    if (existing.inventory_slot == item.inventory_slot ||
        (item.equip_slot && existing.equip_slot == item.equip_slot)) {
      return false;
    }
  }

  return true;
}

void backfill_starter_items(pqxx::work &transaction, Character &character) {
  if (starter_gear_for_class(character.class_id).empty()) {
    return;
  }

  for (auto item : missing_starter_items(character)) {
    const auto rows = transaction.exec(
        "INSERT INTO emulator_character_items(character_id,item_cms_id,inventory_slot,"
        "equip_slot,quantity,durability,grade) VALUES ($1,$2,$3,$4,$5,$6,$7) "
        "ON CONFLICT DO NOTHING RETURNING id",
        pqxx::params{character.id, item.cms_id, item.inventory_slot, item.equip_slot,
                     item.quantity, item.durability, item.grade});
    if (!rows.empty()) {
      item.id = rows.front()[0].as<std::int64_t>();
      character.items.push_back(item);
    }
  }

  std::ranges::sort(character.items, [](const auto &left, const auto &right) {
    return left.inventory_slot < right.inventory_slot;
  });
  transaction.exec("UPDATE emulator_characters SET starter_gear_version=1 WHERE id=$1",
                   pqxx::params{character.id});
}

} // namespace

namespace detail {

bool update_items_in_transaction(pqxx::work &transaction, std::int64_t account_id,
                                 std::int64_t character_id, int world_id,
                                 const std::vector<CharacterItem> &items) {
  // Vacate equipped slots before filling them to honor the partial unique index.
  std::vector<const CharacterItem *> ordered;
  ordered.reserve(items.size());
  for (const auto &item : items) {
    if (!item.equip_slot) {
      ordered.push_back(&item);
    }
  }
  for (const auto &item : items) {
    if (item.equip_slot) {
      ordered.push_back(&item);
    }
  }

  for (const auto *item : ordered) {
    const auto rows = transaction.exec(
        "UPDATE emulator_character_items AS item SET inventory_slot=$5,equip_slot=$6,"
        "quantity=$7,durability=$8,grade=$9 FROM emulator_characters AS character "
        "WHERE item.id=$4 AND item.item_cms_id=$10 AND item.character_id=character.id "
        "AND character.account_id=$1 AND character.id=$2 AND character.world_id=$3 "
        "RETURNING item.id",
        pqxx::params{account_id, character_id, world_id, item->id, item->inventory_slot,
                     item->equip_slot, item->quantity, item->durability, item->grade,
                     item->cms_id});
    if (rows.empty()) {
      return false;
    }
  }

  return true;
}

} // namespace detail

void validate_character_item(const CharacterItem &item) {
  if (item.cms_id <= 0 || item.inventory_slot < 0 ||
      item.inventory_slot > kMaximumInventorySlot ||
      (item.equip_slot && (*item.equip_slot < 0 || *item.equip_slot >= kEquipSlotCount)) ||
      item.quantity <= 0 || item.durability < 0 || item.grade < 0 || item.grade > 255) {
    throw std::invalid_argument("Character item is outside supported storage range");
  }
}

std::string ascii_lower(std::string value) {
  for (char &character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + kAsciiUpperToLowerOffset);
    }
  }

  return value;
}

std::string_view account_role_name(AccountRole role) {
  switch (role) {
  case AccountRole::player:
    return "player";
  case AccountRole::game_master:
    return "gm";
  }

  throw std::invalid_argument("Unknown account role");
}

std::optional<AccountRole> parse_account_role(std::string_view text) {
  const auto normalized = ascii_lower(std::string(text));
  if (normalized == "player") {
    return AccountRole::player;
  }
  if (normalized == "gm" || normalized == "game_master") {
    return AccountRole::game_master;
  }

  return std::nullopt;
}

std::vector<CharacterItem> missing_starter_items(const Character &character) {
  std::vector<CharacterItem> pending;
  std::vector<CharacterItem> occupied = character.items;

  for (auto item : starter_items(character.class_id)) {
    const bool already_owned = std::ranges::any_of(occupied, [&item](const auto &existing) {
      return existing.cms_id == item.cms_id;
    });
    const bool equip_slot_occupied = std::ranges::any_of(occupied, [&item](const auto &existing) {
      return existing.equip_slot == item.equip_slot;
    });
    if (already_owned || equip_slot_occupied) {
      continue;
    }

    while (item.inventory_slot <= 999 &&
           std::ranges::any_of(occupied, [&item](const auto &existing) {
             return existing.inventory_slot == item.inventory_slot;
           })) {
      ++item.inventory_slot;
    }
    if (item.inventory_slot > 999) {
      continue;
    }

    pending.push_back(item);
    occupied.push_back(item);
  }

  return pending;
}

std::optional<Account> MemoryAccountRepository::account(const std::string &name) {
  std::scoped_lock lock(database_->mutex);
  for (const auto &account : database_->accounts) {
    if (account.name == name) {
      return account;
    }
  }

  return {};
}

bool MemoryAccountRepository::save_reconnect_credential(std::int64_t account_id,
                                                        const std::string &digest,
                                                        std::int64_t expires_at) {
  std::scoped_lock lock(database_->mutex);
  const auto account = std::find_if(database_->accounts.begin(), database_->accounts.end(),
                                    [account_id](const Account &candidate) {
                                      return candidate.id == account_id && !candidate.banned;
                                    });
  if (account == database_->accounts.end()) {
    return false;
  }

  std::erase_if(database_->reconnect_credentials, [account_id](const auto &entry) {
    return entry.second.account_id == account_id;
  });
  database_->reconnect_credentials[digest] =
      MemoryDatabase::ReconnectCredential{account_id, expires_at};
  return true;
}

std::optional<Account> MemoryAccountRepository::reconnect_account(const std::string &digest,
                                                                    std::int64_t now) {
  std::scoped_lock lock(database_->mutex);
  const auto credential = database_->reconnect_credentials.find(digest);
  if (credential == database_->reconnect_credentials.end() ||
      credential->second.expires_at <= now) {
    return std::nullopt;
  }

  for (const auto &account : database_->accounts) {
    if (account.id == credential->second.account_id && !account.banned) {
      return account;
    }
  }

  return std::nullopt;
}

void MemoryAccountRepository::revoke_reconnect_credentials(std::int64_t account_id) {
  std::scoped_lock lock(database_->mutex);
  std::erase_if(database_->reconnect_credentials, [account_id](const auto &entry) {
    return entry.second.account_id == account_id;
  });
}

bool MemoryAccountRepository::add_account(Account account) {
  std::scoped_lock lock(database_->mutex);
  for (const auto &existing : database_->accounts) {
    if (existing.name == account.name) {
      return false;
    }
  }

  account.id = static_cast<std::int64_t>(database_->accounts.size()) + 1;
  database_->accounts.push_back(std::move(account));
  return true;
}

bool MemoryAccountRepository::set_account_role(const std::string &name, AccountRole role) {
  std::scoped_lock lock(database_->mutex);
  for (auto &account : database_->accounts) {
    if (account.name == name) {
      account.role = role;
      return true;
    }
  }

  return false;
}

std::vector<Character> MemoryCharacterRepository::characters(std::int64_t account_id,
                                                             int          world_id) {
  std::scoped_lock       lock(database_->mutex);
  std::vector<Character> result;

  for (const auto &character : database_->characters) {
    if (character.account_id == account_id && character.world_id == world_id) {
      result.push_back(character);
    }
  }

  std::ranges::sort(result,
                    [](const auto &left, const auto &right) {
                      return left.slot < right.slot;
                    });
  return result;
}

Character MemoryCharacterRepository::create(Character character) {
  std::scoped_lock lock(database_->mutex);
  for (const auto &existing : database_->characters) {
    if (existing.world_id == character.world_id) {
      // Preserve conflict precedence: name conflicts are checked before slot conflicts.
      if (ascii_lower(existing.name) == ascii_lower(character.name)) {
        throw CreateError(CreateFailure::name_used);
      }

      if (existing.account_id == character.account_id && existing.slot == character.slot) {
        throw CreateError(CreateFailure::slot_full);
      }
    }
  }

  // Marketplace receipts retain deleted participant IDs. Never reuse them.
  character.id = database_->next_character_id++;
  character.items = starter_items(character.class_id);
  for (auto &item : character.items) {
    item.id = database_->next_item_id++;
  }
  database_->characters.push_back(character);
  return character;
}

bool MemoryCharacterRepository::save_world_position(std::int64_t       account_id,
                                                      std::int64_t       character_id,
                                                      int                world_id,
                                                      const WorldPosition &position) {
  validate_world_position(position);

  std::scoped_lock lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id == character_id && character.account_id == account_id &&
        character.world_id == world_id) {
      character.world_position = position;
      return true;
    }
  }

  return false;
}

bool MemoryCharacterRepository::save_progress(std::int64_t account_id,
                                                std::int64_t character_id, int world_id,
                                                int level, std::int64_t experience) {
  validate_progress(level, experience);

  std::scoped_lock lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id == character_id && character.account_id == account_id &&
        character.world_id == world_id) {
      character.level      = level;
      character.experience = experience;
      return true;
    }
  }

  return false;
}

std::optional<CharacterItem>
MemoryCharacterStateRepository::grant_item(std::int64_t  account_id,
                                             std::int64_t  character_id, int world_id,
                                             CharacterItem item) {
  validate_character_item(item);

  std::scoped_lock lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id != character_id || character.account_id != account_id ||
        character.world_id != world_id) {
      continue;
    }

    item.id = 0;
    if (!item_slots_available(character, item)) {
      return std::nullopt;
    }

    item.id = database_->next_item_id++;
    character.items.push_back(item);
    return item;
  }

  return std::nullopt;
}

bool MemoryCharacterStateRepository::update_item(std::int64_t       account_id,
                                                   std::int64_t       character_id,
                                                   int                world_id,
                                                   const CharacterItem &item) {
  validate_character_item(item);

  std::scoped_lock lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id != character_id || character.account_id != account_id ||
        character.world_id != world_id) {
      continue;
    }

    for (auto &existing : character.items) {
      if (existing.id == item.id && existing.cms_id == item.cms_id &&
          item_slots_available(character, item)) {
        existing = item;
        return true;
      }
    }
  }

  return false;
}

bool MemoryCharacterStateRepository::update_items(std::int64_t account_id,
                                                    std::int64_t character_id, int world_id,
                                                    std::int64_t expected_revision,
                                                    const std::vector<CharacterItem> &items) {
  if (!valid_world_revision(expected_revision) || items.empty()) {
    return false;
  }
  for (const auto &item : items) {
    validate_character_item(item);
  }

  std::scoped_lock lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id != character_id || character.account_id != account_id ||
        character.world_id != world_id) {
      continue;
    }

    auto &progress = database_->world_progress[character_id];
    if (progress.revision != expected_revision ||
        !apply_item_updates(character, items)) {
      return false;
    }

    ++progress.revision;
    return true;
  }

  return false;
}

bool MemoryCharacterStateRepository::commit_world_death(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    std::int64_t expected_revision,
    const std::vector<EquippedDurabilityChange> &durability_changes) {
  if (!valid_world_revision(expected_revision) || !valid_durability_changes(durability_changes)) {
    return false;
  }

  std::scoped_lock lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id != character_id || character.account_id != account_id ||
        character.world_id != world_id) {
      continue;
    }

    auto &progress = database_->world_progress[character_id];
    if (progress.revision != expected_revision || progress.dead) {
      return false;
    }

    for (const auto &change : durability_changes) {
      const auto found = std::ranges::find_if(
          character.items, [&](const CharacterItem &item) { return item.id == change.item_id; });
      if (found == character.items.end() || found->cms_id != change.cms_id ||
          found->equip_slot != change.equip_slot ||
          found->durability != change.expected_durability) {
        return false;
      }
    }

    for (const auto &change : durability_changes) {
      const auto found = std::ranges::find_if(
          character.items, [&](const CharacterItem &item) { return item.id == change.item_id; });
      found->durability = change.durability;
    }
    progress.dead = true;
    ++progress.revision;
    return true;
  }

  return false;
}

bool MemoryCharacterStateRepository::commit_world_revival(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    std::int64_t expected_revision, const WorldPosition &position) {
  validate_world_position(position);
  if (!valid_world_revision(expected_revision)) {
    return false;
  }

  std::scoped_lock lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id != character_id || character.account_id != account_id ||
        character.world_id != world_id) {
      continue;
    }

    auto &progress = database_->world_progress[character_id];
    if (progress.revision != expected_revision || !progress.dead) {
      return false;
    }

    character.world_position = position;
    progress.dead = false;
    ++progress.revision;
    return true;
  }

  return false;
}

bool MemoryCharacterStateRepository::commit_world_shrine_revival(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const WorldProgress &before, const WorldProgress &after, const WorldPosition &position) {
  validate_shrine_revival(before, after, position);
  auto committed_progress = after;
  auto committed_build = after.build;

  std::scoped_lock lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id != character_id || character.account_id != account_id ||
        character.world_id != world_id) {
      continue;
    }

    auto &progress = database_->world_progress[character_id];
    if (progress.revision != before.revision || !progress.dead) {
      return false;
    }

    progress = std::move(committed_progress);
    character.world_position = position;
    character.level = after.level;
    character.experience = after.experience;
    character.build = std::move(committed_build);
    character.nui_blessing_level = after.nui_blessing_level;
    character.dedicated_nui_tears = after.dedicated_nui_tears;
    return true;
  }

  return false;
}

bool MemoryCharacterStateRepository::fence_world_entry(std::int64_t account_id,
                                                        std::int64_t character_id, int world_id) {
  std::scoped_lock lock(database_->mutex);
  for (const auto &character : database_->characters) {
    if (character.id != character_id || character.account_id != account_id ||
        character.world_id != world_id) {
      continue;
    }
    auto &progress = database_->world_progress[character_id];
    if (progress.revision == std::numeric_limits<std::int64_t>::max()) {
      return false;
    }
    ++progress.revision;
    return true;
  }
  return false;
}

bool MemoryCharacterStateRepository::commit_world_progress_and_position(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const WorldProgress &before, const WorldProgress &after, const WorldPosition &position) {
  validate_world_position(position);
  detail::validate_world_progress_transition(before, after);
  auto committed_progress = after;
  auto committed_build = after.build;

  std::scoped_lock lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id != character_id || character.account_id != account_id ||
        character.world_id != world_id) {
      continue;
    }

    auto &progress = database_->world_progress[character_id];
    if (progress.revision != before.revision || progress.dead != before.dead) {
      return false;
    }

    progress = std::move(committed_progress);
    character.world_position = position;
    character.level = after.level;
    character.experience = after.experience;
    character.build = std::move(committed_build);
    character.nui_blessing_level = after.nui_blessing_level;
    character.dedicated_nui_tears = after.dedicated_nui_tears;
    return true;
  }
  return false;
}

bool MemoryCharacterStateRepository::discard_items(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    std::int64_t expected_revision, const std::vector<std::int64_t> &item_ids,
    const std::map<int, int> &stackable_counts) {
  if (!valid_discard_request(expected_revision, item_ids, stackable_counts)) {
    return false;
  }

  std::scoped_lock lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id != character_id || character.account_id != account_id ||
        character.world_id != world_id) {
      continue;
    }

    auto &progress = database_->world_progress[character_id];
    if (progress.revision != expected_revision) {
      return false;
    }
    for (const auto item_id : item_ids) {
      const auto found = std::ranges::find_if(character.items, [item_id](const auto &item) {
        return item.id == item_id;
      });
      if (found == character.items.end() || found->equip_slot) {
        return false;
      }
    }
    for (const auto &[cms_id, count] : stackable_counts) {
      const auto found = progress.stackables.find(cms_id);
      if (found == progress.stackables.end() || found->second < count) {
        return false;
      }
    }

    std::erase_if(character.items, [&](const auto &item) {
      return std::ranges::find(item_ids, item.id) != item_ids.end();
    });
    for (const auto &[cms_id, count] : stackable_counts) {
      progress.stackables.at(cms_id) -= count;
    }
    ++progress.revision;
    return true;
  }
  return false;
}

bool MemoryCharacterStateRepository::apply_item_polish(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    std::int64_t expected_revision, const ItemPolishChange &change) {
  if (!valid_polish_change(expected_revision, change)) {
    return false;
  }

  std::scoped_lock lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id != character_id || character.account_id != account_id ||
        character.world_id != world_id) {
      continue;
    }

    auto &progress = database_->world_progress[character_id];
    const auto item = std::ranges::find_if(character.items, [&](const auto &candidate) {
      return candidate.id == change.item_id && candidate.cms_id == change.cms_id;
    });
    const auto material = progress.stackables.find(change.material_cms_id);
    if (progress.revision != expected_revision || item == character.items.end() ||
        item->polish_step != change.expected_polish_step || material == progress.stackables.end() ||
        material->second < change.material_count || progress.gold < change.gold_cost) {
      return false;
    }

    auto additive = progress.stackables.end();
    if (change.additive_cms_id != 0) {
      additive = progress.stackables.find(change.additive_cms_id);
      if (additive == progress.stackables.end() || additive->second < 1) {
        return false;
      }
    }
    if (change.outcome == PolishOutcome::destruction && item->equip_slot) {
      return false;
    }

    // Every check passed; apply the outcome and the charges together.
    if (change.outcome == PolishOutcome::success) {
      item->polish_step = change.expected_polish_step + 1;
    } else if (change.outcome == PolishOutcome::destruction) {
      character.items.erase(item);
    }
    material->second -= change.material_count;
    if (additive != progress.stackables.end()) {
      additive->second -= 1;
    }
    progress.gold -= change.gold_cost;
    ++progress.revision;
    return true;
  }
  return false;
}

bool MemoryCharacterStateRepository::remove_item(std::int64_t account_id,
                                                   std::int64_t character_id, int world_id,
                                                   std::int64_t item_id) {
  std::scoped_lock lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id != character_id || character.account_id != account_id ||
        character.world_id != world_id) {
      continue;
    }

    const auto old_size = character.items.size();
    std::erase_if(character.items, [item_id](const CharacterItem &item) {
      return item.id == item_id;
    });
    return character.items.size() != old_size;
  }

  return false;
}

void migrate_postgres(const std::string &connection_string, const std::string &sql) {
  pqxx::connection connection(connection_string);
  pqxx::work       transaction(connection);

  // Serialize migrations within the same transaction as the schema changes.
  transaction.exec("SELECT pg_advisory_xact_lock(7342091001)");
  transaction.exec(sql);
  transaction.commit();
}

std::optional<Account> PostgresAccountRepository::account(const std::string &name) {
  pqxx::connection       connection(connection_);
  pqxx::read_transaction transaction(connection);
  auto                   rows = transaction.exec(
      "SELECT id,account_name,password_salt,password_hash,password_iterations,account_role,"
      "banned FROM emulator_accounts WHERE account_name=$1",
      pqxx::params{name});
  if (rows.empty()) {
    return {};
  }

  const auto &row = rows.front();
  const auto stored_role = row[5].as<int>();
  if (stored_role != static_cast<int>(AccountRole::player) &&
      stored_role != static_cast<int>(AccountRole::game_master)) {
    throw std::runtime_error("Account has an unknown stored role");
  }

  return Account{row[0].as<std::int64_t>(), row[1].as<std::string>(), row[2].as<std::string>(),
                 row[3].as<std::string>(), row[4].as<int>(),
                 static_cast<AccountRole>(stored_role), row[6].as<bool>()};
}

bool PostgresAccountRepository::save_reconnect_credential(std::int64_t account_id,
                                                        const std::string &digest,
                                                        std::int64_t expires_at) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  const auto accounts = transaction.exec(
      "SELECT id FROM emulator_accounts WHERE id=$1 AND NOT banned FOR UPDATE",
      pqxx::params{account_id});
  if (accounts.empty()) {
    return false;
  }

  transaction.exec(
      "INSERT INTO emulator_reconnect_credentials(account_id, credential_hash, expires_at) "
      "VALUES ($1, $2, $3) ON CONFLICT (account_id) DO UPDATE SET "
      "credential_hash=EXCLUDED.credential_hash, expires_at=EXCLUDED.expires_at",
      pqxx::params{account_id, digest, expires_at});
  transaction.commit();
  return true;
}

std::optional<Account> PostgresAccountRepository::reconnect_account(const std::string &digest,
                                                                    std::int64_t now) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows = transaction.exec(
      "SELECT a.id,a.account_name,a.password_salt,a.password_hash,a.password_iterations,"
      "a.account_role,a.banned FROM emulator_reconnect_credentials r "
      "JOIN emulator_accounts a ON a.id=r.account_id "
      "WHERE r.credential_hash=$1 AND r.expires_at>$2 AND NOT a.banned",
      pqxx::params{digest, now});
  if (rows.empty()) {
    return std::nullopt;
  }

  const auto &row = rows.front();
  const auto role = row[5].as<int>();
  if (role != static_cast<int>(AccountRole::player) &&
      role != static_cast<int>(AccountRole::game_master)) {
    throw std::runtime_error("Account has an unknown stored role");
  }

  return Account{row[0].as<std::int64_t>(), row[1].as<std::string>(), row[2].as<std::string>(),
                 row[3].as<std::string>(), row[4].as<int>(), static_cast<AccountRole>(role),
                 row[6].as<bool>()};
}

void PostgresAccountRepository::revoke_reconnect_credentials(std::int64_t account_id) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  transaction.exec("DELETE FROM emulator_reconnect_credentials WHERE account_id=$1",
                   pqxx::params{account_id});
  transaction.commit();
}

bool PostgresAccountRepository::add_account(Account account) {
  pqxx::connection connection(connection_);
  pqxx::work       transaction(connection);
  auto             rows = transaction.exec(
      "INSERT INTO emulator_accounts(account_name,password_salt,password_hash,password_iterations,"
      "account_role) VALUES ($1,$2,$3,$4,$5) ON CONFLICT DO NOTHING RETURNING id",
      pqxx::params{account.name, account.salt, account.hash, account.iterations,
                   static_cast<int>(account.role)});

  const bool inserted = !rows.empty();
  transaction.commit();
  return inserted;
}

bool PostgresAccountRepository::set_account_role(const std::string &name, AccountRole role) {
  pqxx::connection connection(connection_);
  pqxx::work       transaction(connection);
  const auto       rows = transaction.exec(
      "UPDATE emulator_accounts SET account_role=$2 WHERE account_name=$1 RETURNING id",
      pqxx::params{name, static_cast<int>(role)});

  const bool updated = !rows.empty();
  transaction.commit();
  return updated;
}

std::vector<Character> PostgresCharacterRepository::characters(std::int64_t account_id,
                                                             int world_id) {
  pqxx::connection connection(connection_);
  pqxx::work       transaction(connection);
  // Character and item rows must come from the same snapshot. A concurrent
  // character creation otherwise appears only in the second query.
  transaction.exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
  const auto rows = transaction.exec(
      "SELECT id,account_id,world_id,lobby_slot,name,class_id,level,customization_hex,"
      "world_map_id,world_pos_x_mm,world_pos_y_mm,world_pos_z_mm,world_yaw,experience,"
      "starter_gear_version,nui_blessing_level,dedicated_nui_tears "
      "FROM emulator_characters WHERE account_id=$1 AND world_id=$2 "
      "ORDER BY lobby_slot FOR UPDATE",
      pqxx::params{account_id, world_id});

  std::vector<Character>                        result;
  std::vector<bool>                             needs_starter_backfill;
  std::unordered_map<std::int64_t, std::size_t> character_indexes;
  for (const auto &row : rows) {
    Character character{row[0].as<std::int64_t>(), row[1].as<std::int64_t>(), row[2].as<int>(),
                        row[3].as<int>(), row[5].as<int>(), row[6].as<int>(),
                        row[4].as<std::string>(), row[7].as<std::string>()};
    if (!row[8].is_null() && !row[9].is_null() && !row[10].is_null() &&
        !row[11].is_null() && !row[12].is_null()) {
      character.world_position = WorldPosition{row[8].as<int>(), row[9].as<int>(),
                                               row[10].as<int>(), row[11].as<int>(),
                                               row[12].as<float>()};
    }

    character.experience = row[13].as<std::int64_t>();
    character.nui_blessing_level = row[15].as<int>();
    character.dedicated_nui_tears = row[16].as<std::int32_t>();
    character_indexes.emplace(character.id, result.size());
    needs_starter_backfill.push_back(row[14].as<int>() == 0);
    result.push_back(std::move(character));
  }

  const auto item_rows = transaction.exec(
      "SELECT i.character_id,i.id,i.item_cms_id,i.inventory_slot,i.equip_slot,"
      "i.quantity,i.durability,i.grade,i.created_epoch_ticks,i.polish_step FROM emulator_character_items i "
      "JOIN emulator_characters c ON c.id=i.character_id "
      "WHERE c.account_id=$1 AND c.world_id=$2 "
      "ORDER BY i.character_id,i.inventory_slot",
      pqxx::params{account_id, world_id});
  for (const auto &row : item_rows) {
    const auto character_id    = row[0].as<std::int64_t>();
    const auto character_index = character_indexes.find(character_id);
    if (character_index == character_indexes.end()) {
      throw std::runtime_error("Inventory row refers to an unloaded character");
    }

    CharacterItem item;
    item.id             = row[1].as<std::int64_t>();
    item.cms_id         = row[2].as<int>();
    item.inventory_slot = row[3].as<int>();
    if (!row[4].is_null()) {
      item.equip_slot = row[4].as<int>();
    }
    item.quantity   = row[5].as<int>();
    item.durability = row[6].as<int>();
    item.grade      = row[7].as<int>();
    item.created_epoch_ticks = row[8].as<std::int64_t>();
    item.polish_step = row[9].as<int>();
    result[character_index->second].items.push_back(item);
  }

  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index].build = detail::read_character_build(transaction, result[index].id);
    if (needs_starter_backfill[index]) {
      backfill_starter_items(transaction, result[index]);
    }
  }

  transaction.commit();
  return result;
}

Character PostgresCharacterRepository::create(Character character) {
  try {
    pqxx::connection connection(connection_);
    pqxx::work       transaction(connection);
    auto             rows = transaction.exec(
        "INSERT INTO "
        "emulator_characters(account_id,world_id,lobby_slot,name,class_id,level,"
        "customization_hex,experience,starter_gear_version) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,1) RETURNING id",
        pqxx::params{character.account_id, character.world_id, character.slot, character.name,
                     character.class_id, character.level, character.customization_hex,
                     character.experience});

    character.id    = rows.front()[0].as<std::int64_t>();
    character.items = starter_items(character.class_id);
    for (auto &item : character.items) {
      const auto item_rows = transaction.exec(
          "INSERT INTO emulator_character_items(character_id,item_cms_id,inventory_slot,"
          "equip_slot,quantity,durability,grade) VALUES ($1,$2,$3,$4,$5,$6,$7) RETURNING id",
          pqxx::params{character.id, item.cms_id, item.inventory_slot, item.equip_slot,
                       item.quantity, item.durability, item.grade});
      item.id = item_rows.front()[0].as<std::int64_t>();
    }

    transaction.commit();
    return character;
  } catch (const pqxx::unique_violation &error) {
    // libpq exposes constraint identifiers; names themselves and parameters never reach logs.
    const auto constraint = error.what();
    throw CreateError(std::string(constraint).find("emulator_character_name") != std::string::npos
                        ? CreateFailure::name_used
                        : CreateFailure::slot_full);
  }
}

bool PostgresCharacterRepository::save_world_position(std::int64_t       account_id,
                                                      std::int64_t       character_id,
                                                      int                world_id,
                                                      const WorldPosition &position) {
  validate_world_position(position);

  pqxx::connection connection(connection_);
  pqxx::work       transaction(connection);
  const auto       rows = transaction.exec(
      "UPDATE emulator_characters SET world_map_id=$4,world_pos_x_mm=$5,"
      "world_pos_y_mm=$6,world_pos_z_mm=$7,world_yaw=$8,world_position_saved_at=now() "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 RETURNING id",
      pqxx::params{account_id, character_id, world_id, position.map_id,
                   position.x_mm, position.y_mm, position.z_mm, position.yaw});
  transaction.commit();
  return !rows.empty();
}

bool PostgresCharacterRepository::save_progress(std::int64_t account_id,
                                                std::int64_t character_id, int world_id,
                                                int level, std::int64_t experience) {
  validate_progress(level, experience);

  pqxx::connection connection(connection_);
  pqxx::work       transaction(connection);
  const auto       rows = transaction.exec(
      "UPDATE emulator_characters SET level=$4,experience=$5 "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 RETURNING id",
      pqxx::params{account_id, character_id, world_id, level, experience});
  transaction.commit();
  return !rows.empty();
}

std::optional<CharacterItem>
PostgresCharacterStateRepository::grant_item(std::int64_t  account_id,
                                             std::int64_t  character_id, int world_id,
                                             CharacterItem item) {
  validate_character_item(item);

  pqxx::connection connection(connection_);
  pqxx::work       transaction(connection);
  const auto       rows = transaction.exec(
      "INSERT INTO emulator_character_items(character_id,item_cms_id,inventory_slot,"
      "equip_slot,quantity,durability,grade,created_epoch_ticks,polish_step) "
      "SELECT id,$4,$5,$6,$7,$8,$9,$10,$11 FROM emulator_characters "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 "
      "ON CONFLICT DO NOTHING RETURNING emulator_character_items.id",
      pqxx::params{account_id, character_id, world_id, item.cms_id, item.inventory_slot,
                   item.equip_slot, item.quantity, item.durability, item.grade,
                   item.created_epoch_ticks, item.polish_step});
  transaction.commit();
  if (rows.empty()) {
    return std::nullopt;
  }

  item.id = rows.front()[0].as<std::int64_t>();
  return item;
}

bool PostgresCharacterStateRepository::update_item(std::int64_t       account_id,
                                                   std::int64_t       character_id,
                                                   int                world_id,
                                                   const CharacterItem &item) {
  validate_character_item(item);

  try {
    pqxx::connection connection(connection_);
    pqxx::work       transaction(connection);
    const auto       rows = transaction.exec(
        "UPDATE emulator_character_items AS item SET inventory_slot=$5,equip_slot=$6,"
        "quantity=$7,durability=$8,grade=$9 FROM emulator_characters AS character "
        "WHERE item.id=$4 AND item.item_cms_id=$10 AND item.character_id=character.id "
        "AND character.account_id=$1 AND character.id=$2 AND character.world_id=$3 "
        "RETURNING item.id",
        pqxx::params{account_id, character_id, world_id, item.id, item.inventory_slot,
                     item.equip_slot, item.quantity, item.durability, item.grade, item.cms_id});
    transaction.commit();
    return !rows.empty();
  } catch (const pqxx::unique_violation &) {
    return false;
  }
}

bool PostgresCharacterStateRepository::apply_item_polish(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    std::int64_t expected_revision, const ItemPolishChange &change) {
  if (!valid_polish_change(expected_revision, change)) {
    return false;
  }

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  // The revision guard, the gold charge and the revision bump are one conditional UPDATE.
  const auto owner = transaction.exec(
      "UPDATE emulator_characters SET gameplay_revision=gameplay_revision+1,gold=gold-$5 "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 AND gameplay_revision=$4 AND gold >= $5 "
      "RETURNING id",
      pqxx::params{account_id, character_id, world_id, expected_revision, change.gold_cost});
  if (owner.empty()) {
    return false;
  }

  const auto item_changed = transaction.exec(polish_outcome_statement(change.outcome),
                                             pqxx::params{change.item_id, character_id,
                                                          change.cms_id,
                                                          change.expected_polish_step});
  if (item_changed.empty()) {
    return false;
  }

  const auto charged = transaction.exec(
      "UPDATE emulator_character_stackables SET quantity=quantity-$3 "
      "WHERE character_id=$1 AND item_cms_id=$2 AND quantity >= $3 RETURNING quantity",
      pqxx::params{character_id, change.material_cms_id, change.material_count});
  if (charged.empty()) {
    return false;
  }

  if (change.additive_cms_id != 0) {
    const auto additive_charged = transaction.exec(
        "UPDATE emulator_character_stackables SET quantity=quantity-1 "
        "WHERE character_id=$1 AND item_cms_id=$2 AND quantity >= 1 RETURNING quantity",
        pqxx::params{character_id, change.additive_cms_id});
    if (additive_charged.empty()) {
      return false;
    }
  }

  transaction.commit();
  return true;
}

bool PostgresCharacterStateRepository::remove_item(std::int64_t account_id,
                                                   std::int64_t character_id, int world_id,
                                                   std::int64_t item_id) {
  pqxx::connection connection(connection_);
  pqxx::work       transaction(connection);
  const auto       rows = transaction.exec(
      "DELETE FROM emulator_character_items AS item USING emulator_characters AS character "
      "WHERE item.id=$4 AND item.character_id=character.id "
      "AND character.account_id=$1 AND character.id=$2 AND character.world_id=$3 "
      "RETURNING item.id",
      pqxx::params{account_id, character_id, world_id, item_id});
  transaction.commit();
  return !rows.empty();
}

bool PostgresCharacterStateRepository::discard_items(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    std::int64_t expected_revision, const std::vector<std::int64_t> &item_ids,
    const std::map<int, int> &stackable_counts) {
  if (!valid_discard_request(expected_revision, item_ids, stackable_counts)) {
    return false;
  }

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  const auto owner = transaction.exec(
      "UPDATE emulator_characters SET gameplay_revision=gameplay_revision+1 "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 AND gameplay_revision=$4 RETURNING id",
      pqxx::params{account_id, character_id, world_id, expected_revision});
  if (owner.empty()) {
    return false;
  }

  for (const auto item_id : item_ids) {
    const auto deleted = transaction.exec(
        "DELETE FROM emulator_character_items WHERE id=$1 AND character_id=$2 "
        "AND equip_slot IS NULL RETURNING id",
        pqxx::params{item_id, character_id});
    if (deleted.empty()) {
      return false;
    }
  }
  for (const auto &[cms_id, count] : stackable_counts) {
    const auto changed = transaction.exec(
        "UPDATE emulator_character_stackables SET quantity=quantity-$3 "
        "WHERE character_id=$1 AND item_cms_id=$2 AND quantity >= $3 RETURNING quantity",
        pqxx::params{character_id, cms_id, count});
    if (changed.empty()) {
      return false;
    }
  }

  transaction.commit();
  return true;
}

bool PostgresCharacterStateRepository::update_items(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    std::int64_t expected_revision, const std::vector<CharacterItem> &items) {
  if (!valid_world_revision(expected_revision) || items.empty()) {
    return false;
  }
  std::set<std::int64_t> item_ids;
  for (const auto &item : items) {
    validate_character_item(item);
    validate_item(item);
    if (item.id <= 0 || !item_ids.insert(item.id).second) {
      return false;
    }
  }

  try {
    pqxx::connection connection(connection_);
    pqxx::work transaction(connection);
    const auto character = transaction.exec(
        "UPDATE emulator_characters SET gameplay_revision=gameplay_revision+1 "
        "WHERE account_id=$1 AND id=$2 AND world_id=$3 AND gameplay_revision=$4 RETURNING id",
        pqxx::params{account_id, character_id, world_id, expected_revision});
    if (character.empty() ||
        !detail::update_items_in_transaction(transaction, account_id, character_id, world_id,
                                             items)) {
      return false;
    }

    transaction.commit();
    return true;
  } catch (const pqxx::unique_violation &) {
    return false;
  }
}

bool PostgresCharacterStateRepository::commit_world_death(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    std::int64_t expected_revision,
    const std::vector<EquippedDurabilityChange> &durability_changes) {
  if (!valid_world_revision(expected_revision) || !valid_durability_changes(durability_changes)) {
    return false;
  }

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  const auto character = transaction.exec(
      "SELECT id FROM emulator_characters "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 AND gameplay_revision=$4 "
      "AND player_dead=false FOR UPDATE",
      pqxx::params{account_id, character_id, world_id, expected_revision});
  if (character.empty()) {
    return false;
  }

  // Lock every validated item before changing either the character or any durability.
  for (const auto &change : durability_changes) {
    const auto item = transaction.exec(
        "SELECT id FROM emulator_character_items "
        "WHERE character_id=$1 AND id=$2 AND item_cms_id=$3 AND equip_slot=$4 "
        "AND durability=$5 FOR UPDATE",
        pqxx::params{character_id, change.item_id, change.cms_id, change.equip_slot,
                     change.expected_durability});
    if (item.empty()) {
      return false;
    }
  }

  transaction.exec(
      "UPDATE emulator_characters SET gameplay_revision=gameplay_revision+1,player_dead=true "
      "WHERE id=$1",
      pqxx::params{character_id});
  for (const auto &change : durability_changes) {
    transaction.exec("UPDATE emulator_character_items SET durability=$2 WHERE id=$1",
                     pqxx::params{change.item_id, change.durability});
  }

  transaction.commit();
  return true;
}

bool PostgresCharacterStateRepository::commit_world_revival(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    std::int64_t expected_revision, const WorldPosition &position) {
  validate_world_position(position);
  if (!valid_world_revision(expected_revision)) {
    return false;
  }

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  const auto character = transaction.exec(
      "UPDATE emulator_characters SET gameplay_revision=gameplay_revision+1,"
      "player_dead=false,world_map_id=$5,world_pos_x_mm=$6,world_pos_y_mm=$7,"
      "world_pos_z_mm=$8,world_yaw=$9,world_position_saved_at=now() "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 AND gameplay_revision=$4 "
      "AND player_dead=true RETURNING id",
      pqxx::params{account_id, character_id, world_id, expected_revision, position.map_id,
                   position.x_mm, position.y_mm, position.z_mm, position.yaw});
  if (character.empty()) {
    return false;
  }

  transaction.commit();
  return true;
}

bool PostgresCharacterStateRepository::commit_world_shrine_revival(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const WorldProgress &before, const WorldProgress &after, const WorldPosition &position) {
  validate_shrine_revival(before, after, position);

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  // The shared writer acquires the owned character row through its revision/dead guard,
  // then persists all supplied progress. A later failure rolls back those writes as well.
  if (!detail::write_world_progress_transition(transaction, account_id, character_id,
                                               world_id, before, after)) {
    return false;
  }
  const auto revived = transaction.exec(
      "UPDATE emulator_characters SET player_dead=false,world_map_id=$5,world_pos_x_mm=$6,"
      "world_pos_y_mm=$7,world_pos_z_mm=$8,world_yaw=$9,world_position_saved_at=now() "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 AND gameplay_revision=$4 "
      "AND player_dead=true RETURNING id",
      pqxx::params{account_id, character_id, world_id, after.revision, position.map_id,
                   position.x_mm, position.y_mm, position.z_mm, position.yaw});
  if (revived.empty()) {
    return false;
  }

  transaction.commit();
  return true;
}

bool PostgresCharacterStateRepository::fence_world_entry(std::int64_t account_id,
                                                          std::int64_t character_id, int world_id) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  transaction.exec("SET LOCAL lock_timeout = '5s'");
  const auto updated = transaction.exec(
      "UPDATE emulator_characters SET gameplay_revision=gameplay_revision+1 "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 "
      "AND gameplay_revision<9223372036854775807 RETURNING id",
      pqxx::params{account_id, character_id, world_id});
  if (updated.empty()) {
    return false;
  }
  transaction.commit();
  return true;
}

bool PostgresCharacterStateRepository::commit_world_progress_and_position(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const WorldProgress &before, const WorldProgress &after, const WorldPosition &position) {
  validate_world_position(position);
  detail::validate_world_progress_transition(before, after);

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  if (!detail::write_world_progress_transition(transaction, account_id, character_id, world_id,
                                                before, after)) {
    return false;
  }

  const auto updated = transaction.exec(
      "UPDATE emulator_characters SET world_map_id=$5,world_pos_x_mm=$6,world_pos_y_mm=$7,"
      "world_pos_z_mm=$8,world_yaw=$9,world_position_saved_at=now() "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 AND gameplay_revision=$4 RETURNING id",
      pqxx::params{account_id, character_id, world_id, after.revision, position.map_id,
                   position.x_mm, position.y_mm, position.z_mm, position.yaw});
  if (updated.empty()) {
    return false;
  }
  transaction.commit();
  return true;
}

} // namespace chronicles
