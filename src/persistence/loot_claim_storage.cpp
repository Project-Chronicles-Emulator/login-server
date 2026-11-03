#include "persistence/loot_claim_internal.hpp"
#include "persistence/memory_database.hpp"
#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"
#include "persistence/quest_service_effect_guards.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace chronicles {
namespace {

using Json = nlohmann::json;
// The trade contract limits one grant to 64 item rows.
constexpr std::size_t kMaximumStoredLootGrantRows = 64;

bool lowercase_hex(std::string_view value, std::size_t length) {
  return value.size() == length && std::ranges::all_of(value, [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

void validate_claim_identity(std::int64_t account_id, std::int64_t character_id, int world_id,
                             const std::string &receipt_id, const std::string &fingerprint) {
  if (account_id <= 0 || character_id <= 0 || world_id <= 0 ||
      !lowercase_hex(receipt_id, 32) || !lowercase_hex(fingerprint, 64)) {
    throw std::invalid_argument("Invalid durable loot claim identity");
  }
}

LootClaimResult classify_claim(const StoredLootClaim &stored, std::int64_t account_id,
                               std::int64_t character_id, int world_id,
                               const std::string &fingerprint) {
  if (stored.claim_kind != "loot" || stored.world_id != world_id ||
      stored.corpse_fingerprint != fingerprint) {
    return {LootClaimStatus::mismatch, {}};
  }
  if (stored.account_id != account_id || stored.character_id != character_id) {
    return {LootClaimStatus::claimed_by_other_character, {}};
  }
  return {LootClaimStatus::already_claimed_by_same_character, stored.granted_items};
}

Json item_json(const CharacterItem &item) {
  return {{"id", item.id}, {"cms_id", item.cms_id}, {"inventory_slot", item.inventory_slot},
          {"equip_slot", item.equip_slot ? Json(*item.equip_slot) : Json(nullptr)},
          {"quantity", item.quantity}, {"durability", item.durability}, {"grade", item.grade},
          {"polish_step", item.polish_step}, {"created_epoch_ticks", item.created_epoch_ticks}};
}

Json item_rows_json(const std::vector<CharacterItem> &items) {
  auto rows = Json::array();
  for (const auto &item : items) {
    rows.push_back(item_json(item));
  }
  return rows;
}

std::vector<CharacterItem> parse_item_rows(const std::string &text) {
  const auto rows = Json::parse(text);
  if (!rows.is_array() || rows.size() > kMaximumStoredLootGrantRows) {
    throw std::runtime_error("Invalid durable loot item receipt");
  }
  std::vector<CharacterItem> items;
  items.reserve(rows.size());
  for (const auto &row : rows) {
    CharacterItem item;
    item.id = row.at("id").get<std::int64_t>();
    item.cms_id = row.at("cms_id").get<int>();
    item.inventory_slot = row.at("inventory_slot").get<int>();
    if (!row.at("equip_slot").is_null()) {
      item.equip_slot = row.at("equip_slot").get<int>();
    }
    item.quantity = row.at("quantity").get<int>();
    item.durability = row.at("durability").get<int>();
    item.grade = row.at("grade").get<int>();
    item.polish_step = row.at("polish_step").get<int>();
    item.created_epoch_ticks = row.at("created_epoch_ticks").get<std::int64_t>();
    if (item.id <= 0 || item.equip_slot || item.polish_step < 0 ||
        item.polish_step > kMaxItemPolishStep) {
      throw std::runtime_error("Invalid durable loot item receipt");
    }
    validate_character_item(item);
    items.push_back(item);
  }
  return items;
}

StoredLootClaim read_claim(const pqxx::row &row) {
  StoredLootClaim result;
  result.account_id = row[0].as<std::int64_t>();
  result.character_id = row[1].as<std::int64_t>();
  result.world_id = row[2].as<int>();
  result.corpse_fingerprint = row[3].as<std::string>();
  result.granted_items = parse_item_rows(row[4].as<std::string>());
  result.claim_kind = row[5].as<std::string>();
  result.mutation_fingerprint = row[6].is_null() ? "" : row[6].as<std::string>();
  result.terminal = row[7].as<bool>();
  return result;
}

LootClaimResult lookup_postgres_claim(pqxx::transaction_base &transaction,
                                      std::int64_t account_id, std::int64_t character_id,
                                      int world_id, const std::string &receipt_id,
                                      const std::string &fingerprint) {
  const auto owner = transaction.exec(
      "SELECT id FROM emulator_characters WHERE account_id=$1 AND id=$2 AND world_id=$3",
      pqxx::params{account_id, character_id, world_id});
  if (owner.empty()) {
    return {LootClaimStatus::conflict, {}};
  }
  const auto rows = transaction.exec(
      "SELECT account_id, character_id, world_id, corpse_fingerprint, granted_items, "
      "claim_kind, mutation_fingerprint, terminal "
      "FROM emulator_loot_claim_receipts WHERE receipt_id=$1", pqxx::params{receipt_id});
  if (rows.empty()) {
    return {LootClaimStatus::not_found, {}};
  }
  return classify_claim(read_claim(rows.front()), account_id, character_id, world_id,
                        fingerprint);
}

} // namespace

LootClaimResult MemoryCharacterStateRepository::lookup_loot_claim(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &receipt_id, const std::string &corpse_fingerprint) {
  validate_claim_identity(account_id, character_id, world_id, receipt_id, corpse_fingerprint);
  std::lock_guard lock(database_->mutex);
  if (detail::find_owned_character(*database_, account_id, character_id, world_id) ==
      database_->characters.end()) {
    return {LootClaimStatus::conflict, {}};
  }
  const auto found = database_->loot_claims.find(receipt_id);
  if (found == database_->loot_claims.end()) {
    return {LootClaimStatus::not_found, {}};
  }
  return classify_claim(found->second, account_id, character_id, world_id, corpse_fingerprint);
}

LootClaimResult MemoryCharacterStateRepository::commit_loot_claim(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &receipt_id, const std::string &corpse_fingerprint,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade) {
  validate_claim_identity(account_id, character_id, world_id, receipt_id, corpse_fingerprint);
  detail::validate_loot_trade(before, after, trade);
  std::lock_guard lock(database_->mutex);
  const auto owner_epoch = monster_reward_owner_epoch_.load();
  if (owner_epoch > 0 && database_->monster_reward_owner_epoch != owner_epoch) {
    throw std::runtime_error("Reward owner epoch is stale or missing");
  }
  const auto owner = detail::find_owned_character(*database_, account_id, character_id, world_id);
  if (owner == database_->characters.end()) {
    return {LootClaimStatus::conflict, {}};
  }
  const auto previous = database_->loot_claims.find(receipt_id);
  if (previous != database_->loot_claims.end()) {
    return classify_claim(previous->second, account_id, character_id, world_id,
                          corpse_fingerprint);
  }
  auto [capacity, inserted_capacity] = database_->loot_claim_counts.try_emplace(world_id, 0);
  static_cast<void>(inserted_capacity);
  if (capacity->second >= kMaximumRetainedLootClaimsPerWorld) {
    return {LootClaimStatus::conflict, {}};
  }
  auto &stored_progress = database_->world_progress[character_id];
  if (stored_progress.revision != before.revision || stored_progress.dead != before.dead) {
    return {LootClaimStatus::conflict, {}};
  }

  auto prepared = detail::prepare_memory_loot_trade(*database_, *owner, after, trade);
  if (!prepared) {
    return {LootClaimStatus::conflict, {}};
  }
  // Every allocation, including the return value and durable receipt, completes before
  // the shared helper publishes the prepared state using nonthrowing swaps.
  LootClaimResult result{LootClaimStatus::committed, prepared->granted_items};
  auto [receipt, inserted] = database_->loot_claims.emplace(
      receipt_id, StoredLootClaim{account_id, character_id, world_id, corpse_fingerprint,
                                  prepared->granted_items});
  if (!inserted) {
    throw std::logic_error("Loot receipt changed while the memory database was locked");
  }
  ++capacity->second;
  detail::publish_memory_loot_trade(*database_, *owner, stored_progress, *prepared);
  return result;
}

LootClaimResult PostgresCharacterStateRepository::lookup_loot_claim(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &receipt_id, const std::string &corpse_fingerprint) {
  validate_claim_identity(account_id, character_id, world_id, receipt_id, corpse_fingerprint);
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  return lookup_postgres_claim(transaction, account_id, character_id, world_id, receipt_id,
                               corpse_fingerprint);
}

LootClaimResult PostgresCharacterStateRepository::commit_loot_claim(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &receipt_id, const std::string &corpse_fingerprint,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade) {
  validate_claim_identity(account_id, character_id, world_id, receipt_id, corpse_fingerprint);
  detail::validate_loot_trade(before, after, trade);
  try {
    pqxx::connection connection(connection_);
    pqxx::work transaction(connection);
    const auto owner_epoch = monster_reward_owner_epoch_.load();
    if (owner_epoch > 0) {
      // This lock makes takeover wait for this claim and rejects a stale owner before
      // either the receipt or its progress and item changes can be written.
      detail::require_reward_owner_epoch(transaction, owner_epoch);
    }
    const auto inserted = transaction.exec(
        "INSERT INTO emulator_loot_claim_receipts "
        "(receipt_id, account_id, character_id, world_id, corpse_fingerprint, granted_items) "
        "VALUES ($1,$2,$3,$4,$5,'[]'::jsonb) ON CONFLICT (receipt_id) DO NOTHING "
        "RETURNING receipt_id",
        pqxx::params{receipt_id, account_id, character_id, world_id, corpse_fingerprint});
    if (inserted.empty()) {
      return lookup_postgres_claim(transaction, account_id, character_id, world_id, receipt_id,
                                   corpse_fingerprint);
    }
    transaction.exec("INSERT INTO emulator_loot_claim_limits(world_id,receipt_count) "
                     "VALUES ($1,0) ON CONFLICT DO NOTHING", pqxx::params{world_id});
    const auto capacity = transaction.exec(
        "UPDATE emulator_loot_claim_limits SET receipt_count=receipt_count+1 "
        "WHERE world_id=$1 AND receipt_count < $2 RETURNING receipt_count",
        pqxx::params{world_id, kMaximumRetainedLootClaimsPerWorld});
    if (capacity.empty()) {
      return {LootClaimStatus::conflict, {}};
    }
    auto granted = detail::apply_postgres_loot_trade(transaction, account_id, character_id,
                                                     world_id, before, after, trade);
    if (!granted) {
      return {LootClaimStatus::conflict, {}};
    }
    transaction.exec("UPDATE emulator_loot_claim_receipts SET granted_items=$2::jsonb "
                     "WHERE receipt_id=$1", pqxx::params{receipt_id, item_rows_json(*granted).dump()});
    transaction.commit();
    return {LootClaimStatus::committed, std::move(*granted)};
  } catch (const pqxx::unique_violation &) {
    return {LootClaimStatus::conflict, {}};
  }
}

} // namespace chronicles
