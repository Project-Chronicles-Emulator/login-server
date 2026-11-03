// Operator console storage queries. These are reached only through the authenticated admin
// API (src/services/admin/admin_api.cpp); no game-client request can call them.

#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"
#include "persistence/quest_service_effect_guards.hpp"

#include <algorithm>
#include <unordered_map>

#include <pqxx/pqxx>

namespace chronicles {
namespace {

// ISO-8601 UTC text keeps the admin API independent of the server's local time zone.
constexpr const char *kCreatedAtUtcSql =
    "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"')";

AccountRole stored_account_role(int stored_role) {
  if (stored_role != static_cast<int>(AccountRole::player) &&
      stored_role != static_cast<int>(AccountRole::game_master)) {
    throw std::runtime_error("Account has an unknown stored role");
  }
  return static_cast<AccountRole>(stored_role);
}

// Column order: id, account_id, world_id, lobby_slot, name, class_id, level,
// customization_hex, world_map_id, world_pos_x_mm, world_pos_y_mm, world_pos_z_mm, world_yaw,
// experience, nui_blessing_level, dedicated_nui_tears.
Character character_from_row(const pqxx::row &row) {
  Character character;
  character.id = row[0].as<std::int64_t>();
  character.account_id = row[1].as<std::int64_t>();
  character.world_id = row[2].as<int>();
  character.slot = row[3].as<int>();
  character.name = row[4].as<std::string>();
  character.class_id = row[5].as<int>();
  character.level = row[6].as<int>();
  character.customization_hex = row[7].as<std::string>();

  const bool has_position = !row[8].is_null() && !row[9].is_null() && !row[10].is_null() &&
                            !row[11].is_null() && !row[12].is_null();
  if (has_position) {
    character.world_position = WorldPosition{row[8].as<int>(), row[9].as<int>(), row[10].as<int>(),
                                             row[11].as<int>(), row[12].as<float>()};
  }

  character.experience = row[13].as<std::int64_t>();
  character.nui_blessing_level = row[14].as<int>();
  character.dedicated_nui_tears = row[15].as<std::int32_t>();
  return character;
}

constexpr const char *kCharacterColumnsSql =
    "id,account_id,world_id,lobby_slot,name,class_id,level,customization_hex,"
    "world_map_id,world_pos_x_mm,world_pos_y_mm,world_pos_z_mm,world_yaw,experience,"
    "nui_blessing_level,dedicated_nui_tears";

} // namespace

std::vector<AccountSummary> MemoryAccountRepository::list_accounts() {
  std::scoped_lock lock(database_->mutex);

  std::unordered_map<std::int64_t, int> character_counts;
  for (const auto &character : database_->characters) {
    ++character_counts[character.account_id];
  }

  std::vector<AccountSummary> result;
  result.reserve(database_->accounts.size());
  for (const auto &account : database_->accounts) {
    AccountSummary summary;
    summary.id = account.id;
    summary.name = account.name;
    summary.role = account.role;
    summary.banned = account.banned;
    summary.character_count = character_counts[account.id];
    result.push_back(std::move(summary));
  }
  return result;
}

std::optional<Account> MemoryAccountRepository::account_by_id(std::int64_t account_id) {
  std::scoped_lock lock(database_->mutex);
  for (const auto &account : database_->accounts) {
    if (account.id == account_id) {
      return account;
    }
  }
  return std::nullopt;
}

bool MemoryAccountRepository::set_account_password(std::int64_t account_id,
                                                     const std::string &salt,
                                                     const std::string &hash, int iterations) {
  std::scoped_lock lock(database_->mutex);
  for (auto &account : database_->accounts) {
    if (account.id == account_id) {
      account.salt = salt;
      account.hash = hash;
      account.iterations = iterations;
      std::erase_if(database_->reconnect_credentials, [account_id](const auto &entry) {
        return entry.second.account_id == account_id;
      });
      return true;
    }
  }
  return false;
}

bool MemoryAccountRepository::set_account_banned(std::int64_t account_id, bool banned) {
  std::scoped_lock lock(database_->mutex);
  for (auto &account : database_->accounts) {
    if (account.id == account_id) {
      account.banned = banned;
      if (banned) {
        std::erase_if(database_->reconnect_credentials, [account_id](const auto &entry) {
          return entry.second.account_id == account_id;
        });
      }
      return true;
    }
  }
  return false;
}

std::vector<Character> MemoryCharacterRepository::list_characters() {
  std::scoped_lock lock(database_->mutex);

  std::vector<Character> result;
  result.reserve(database_->characters.size());
  for (const auto &character : database_->characters) {
    auto summary = character;
    summary.items.clear();
    result.push_back(std::move(summary));
  }
  return result;
}

std::optional<Character> MemoryCharacterRepository::character_by_id(std::int64_t character_id) {
  std::scoped_lock lock(database_->mutex);
  for (const auto &character : database_->characters) {
    if (character.id == character_id) {
      return character;
    }
  }
  return std::nullopt;
}

CharacterRenameResult MemoryCharacterRepository::rename_character(std::int64_t character_id,
                                                                    const std::string &name) {
  std::scoped_lock lock(database_->mutex);
  const auto target = std::ranges::find_if(
      database_->characters,
      [character_id](const Character &entry) { return entry.id == character_id; });
  if (target == database_->characters.end()) {
    return CharacterRenameResult::not_found;
  }

  // Names are unique per world, ignoring ASCII case, like the PostgreSQL index.
  const auto lowered_name = ascii_lower(name);
  for (const auto &other : database_->characters) {
    const bool same_world = other.world_id == target->world_id;
    if (other.id != character_id && same_world && ascii_lower(other.name) == lowered_name) {
      return CharacterRenameResult::name_used;
    }
  }

  target->name = name;
  return CharacterRenameResult::renamed;
}

bool MemoryCharacterRepository::delete_character(std::int64_t character_id) {
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  if (database_->quest_services->company_effect_pending(character_id) ||
      database_->quest_services->guild_effect_pending(character_id)) {
    return false;
  }
  const auto skill_receipt = database_->skill_item_uses.find(character_id);
  if (skill_receipt != database_->skill_item_uses.end() &&
      (skill_receipt->second.state == SkillItemUseState::reserved ||
       skill_receipt->second.state == SkillItemUseState::committed)) {
    return false;
  }
  // Features that keep per-character rows (market escrow, fog, mail) may veto the removal or
  // erase their rows once the character is gone. The callbacks run with the mutex held.
  for (const auto &hook : database_->deletion_hooks) {
    if (hook.blocks_deletion && hook.blocks_deletion(character_id)) {
      return false;
    }
  }
  auto remaining_characters = database_->characters;
  const auto removed =
      std::erase_if(remaining_characters, [character_id](const Character &character) {
        return character.id == character_id;
      });
  if (removed == 0) {
    return false;
  }
  auto company_generations = database_->quest_services->company_generations;
  auto guild_generations = database_->quest_services->guild_generations;
  advance_quest_membership_generation(company_generations, character_id);
  advance_quest_membership_generation(guild_generations, character_id);
  // Publish identity removal and generation advances together, after all allocations.
  database_->characters.swap(remaining_characters);
  database_->quest_services->company_generations.swap(company_generations);
  database_->quest_services->guild_generations.swap(guild_generations);
  database_->quest_services->company_memberships.erase(character_id);
  database_->quest_services->guild_memberships.erase(character_id);
  database_->world_progress.erase(character_id);
  database_->skill_item_uses.erase(character_id);
  std::erase_if(database_->monster_rewards, [character_id](const auto &entry) {
    return entry.first.first == character_id;
  });
  for (const auto &hook : database_->deletion_hooks) {
    if (hook.erase_rows) {
      hook.erase_rows(character_id);
    }
  }
  return true;
}

std::vector<AccountSummary> PostgresAccountRepository::list_accounts() {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows =
      transaction.exec(std::string("SELECT a.id,a.account_name,a.account_role,a.banned,") +
                       "(SELECT count(*) FROM emulator_characters c WHERE c.account_id=a.id)," +
                       kCreatedAtUtcSql + " FROM emulator_accounts a ORDER BY a.id");

  std::vector<AccountSummary> result;
  result.reserve(rows.size());
  for (const auto &row : rows) {
    AccountSummary summary;
    summary.id = row[0].as<std::int64_t>();
    summary.name = row[1].as<std::string>();
    summary.role = stored_account_role(row[2].as<int>());
    summary.banned = row[3].as<bool>();
    summary.character_count = row[4].as<int>();
    summary.created_at = row[5].as<std::string>();
    result.push_back(std::move(summary));
  }
  return result;
}

std::optional<Account> PostgresAccountRepository::account_by_id(std::int64_t account_id) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows = transaction.exec(
      "SELECT id,account_name,password_salt,password_hash,password_iterations,account_role,"
      "banned FROM emulator_accounts WHERE id=$1",
      pqxx::params{account_id});
  if (rows.empty()) {
    return std::nullopt;
  }

  const auto &row = rows.front();
  return Account{row[0].as<std::int64_t>(), row[1].as<std::string>(),
                 row[2].as<std::string>(),  row[3].as<std::string>(),
                 row[4].as<int>(),          stored_account_role(row[5].as<int>()),
                 row[6].as<bool>()};
}

bool PostgresAccountRepository::set_account_password(std::int64_t account_id,
                                                     const std::string &salt,
                                                     const std::string &hash, int iterations) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  const auto rows = transaction.exec(
      "UPDATE emulator_accounts SET password_salt=$2,password_hash=$3,password_iterations=$4 "
      "WHERE id=$1 RETURNING id",
      pqxx::params{account_id, salt, hash, iterations});

  const bool updated = !rows.empty();
  if (updated) {
    transaction.exec("DELETE FROM emulator_reconnect_credentials WHERE account_id=$1",
                     pqxx::params{account_id});
  }
  transaction.commit();
  return updated;
}

bool PostgresAccountRepository::set_account_banned(std::int64_t account_id, bool banned) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  const auto rows =
      transaction.exec("UPDATE emulator_accounts SET banned=$2 WHERE id=$1 RETURNING id",
                       pqxx::params{account_id, banned});

  const bool updated = !rows.empty();
  if (updated && banned) {
    transaction.exec("DELETE FROM emulator_reconnect_credentials WHERE account_id=$1",
                     pqxx::params{account_id});
  }
  transaction.commit();
  return updated;
}

std::vector<Character> PostgresCharacterRepository::list_characters() {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows = transaction.exec(std::string("SELECT ") + kCharacterColumnsSql +
                                     " FROM emulator_characters ORDER BY id");

  std::vector<Character> result;
  result.reserve(rows.size());
  for (const auto &row : rows) {
    result.push_back(character_from_row(row));
  }
  return result;
}

std::optional<Character> PostgresCharacterRepository::character_by_id(std::int64_t character_id) {
  std::int64_t account_id{};
  int world_id{};
  {
    pqxx::connection connection(connection_);
    pqxx::read_transaction transaction(connection);
    const auto rows =
        transaction.exec("SELECT account_id,world_id FROM emulator_characters WHERE id=$1",
                         pqxx::params{character_id});
    if (rows.empty()) {
      return std::nullopt;
    }
    account_id = rows.front()[0].as<std::int64_t>();
    world_id = rows.front()[1].as<int>();
  }

  // Reuse the owner query so item loading and the starter-gear backfill stay in one place.
  for (auto &character : characters(account_id, world_id)) {
    if (character.id == character_id) {
      return std::move(character);
    }
  }
  return std::nullopt;
}

CharacterRenameResult
PostgresCharacterRepository::rename_character(std::int64_t character_id,
                                              const std::string &name) {
  try {
    pqxx::connection connection(connection_);
    pqxx::work transaction(connection);
    const auto rows =
        transaction.exec("UPDATE emulator_characters SET name=$2 WHERE id=$1 RETURNING id",
                         pqxx::params{character_id, name});

    const bool renamed = !rows.empty();
    transaction.commit();
    return renamed ? CharacterRenameResult::renamed : CharacterRenameResult::not_found;
  } catch (const pqxx::unique_violation &) {
    return CharacterRenameResult::name_used;
  }
}

bool PostgresCharacterRepository::delete_character(std::int64_t character_id) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  const auto initial = transaction.exec("SELECT id FROM emulator_characters WHERE id=$1",
                                        pqxx::params{character_id});
  if (initial.empty()) {
    return false;
  }
  const auto membership = transaction.exec(
      "SELECT guild_id FROM emulator_guild_members WHERE character_id=$1",
      pqxx::params{character_id});
  const auto guild_id = membership.empty() ? 0 : membership.front()[0].as<std::int64_t>();
  if (guild_id != 0) {
    detail::lock_quest_service_guild(transaction, guild_id);
  }
  detail::lock_quest_service_character(transaction, character_id);
  const auto current_membership = transaction.exec(
      "SELECT guild_id FROM emulator_guild_members WHERE character_id=$1",
      pqxx::params{character_id});
  const auto current_guild =
      current_membership.empty() ? 0 : current_membership.front()[0].as<std::int64_t>();
  if (current_guild != guild_id) {
    return false;
  }
  const auto pending_effects = transaction.exec(
      "SELECT 1 FROM emulator_quest_service_effects WHERE character_id=$1 "
      "AND (NOT company_applied OR NOT guild_applied) LIMIT 1", pqxx::params{character_id});
  if (!pending_effects.empty()) {
    return false;
  }
  // Serialize deletion against registration, purchase and claim before checking escrow.
  transaction.exec("SELECT id FROM emulator_characters WHERE id=$1 FOR UPDATE",
                   pqxx::params{character_id});
  // The inactive continuation experiment is installed explicitly in development.
  // Ordinary schema-44 servers have no such table; experimental owners still fence deletion.
  const auto has_skill_receipts = transaction.exec(
      "SELECT to_regclass('emulator_skill_item_use_receipts') IS NOT NULL");
  if (has_skill_receipts.front()[0].as<bool>()) {
    const auto skill_receipt = transaction.exec(
        "SELECT state FROM emulator_skill_item_use_receipts WHERE character_id=$1 FOR UPDATE",
        pqxx::params{character_id});
    if (!skill_receipt.empty()) {
      const auto state = skill_receipt.front()[0].as<std::string>();
      if (state == "reserved" || state == "committed") {
        return false;
      }
      transaction.exec("DELETE FROM emulator_skill_item_use_receipts WHERE character_id=$1",
                       pqxx::params{character_id});
    }
  }
  const auto pending = transaction.exec(
      "SELECT id FROM emulator_market WHERE (seller_id=$1 AND NOT seller_claimed) OR "
      "(buyer_id=$1 AND NOT buyer_claimed) LIMIT 1", pqxx::params{character_id});
  if (!pending.empty()) {
    return false;
  }
  detail::advance_quest_company_generation(transaction, character_id);
  detail::advance_quest_guild_generation(transaction, character_id);
  // Character-owned rows cascade; durable reward and loot tombstones intentionally survive.
  const auto rows = transaction.exec("DELETE FROM emulator_characters WHERE id=$1 RETURNING id",
                                     pqxx::params{character_id});

  const bool deleted = !rows.empty();
  transaction.commit();
  return deleted;
}

} // namespace chronicles
