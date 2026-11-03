#include "persistence/quest_service_effect_guards.hpp"
#include "persistence/character_state_types.hpp"

#include <stdexcept>

namespace chronicles::detail {

bool reserve_shared_source_capacity(pqxx::work &transaction, int world_id, std::size_t amount) {
  if (world_id <= 0 || amount == 0 || amount > kMaximumSharedGatheringSourcesPerWorld) {
    throw std::invalid_argument("Invalid shared source capacity reservation");
  }
  transaction.exec("INSERT INTO emulator_gathering_source_limits(world_id,source_count) "
                   "VALUES ($1,0) ON CONFLICT DO NOTHING", pqxx::params{world_id});
  const auto updated = transaction.exec(
      "UPDATE emulator_gathering_source_limits SET source_count=source_count+$2 "
      "WHERE world_id=$1 AND source_count <= $3 RETURNING source_count",
      pqxx::params{world_id, amount, kMaximumSharedGatheringSourcesPerWorld - amount});
  return !updated.empty();
}

StoredSharedGatheringSource decode_shared_gathering_source(const pqxx::row &row,
                                                           std::size_t offset) {
  StoredSharedGatheringSource result;
  result.generation = row[offset].as<std::int64_t>();
  result.active = row[offset + 1].as<bool>();
  if (!row[offset + 2].is_null()) {
    result.phase = row[offset + 2].as<int>();
  }
  if (!row[offset + 3].is_null()) {
    result.pending_receipt_id = row[offset + 3].as<std::string>();
  }
  if (!row[offset + 4].is_null()) {
    result.next_transition_epoch_ticks = row[offset + 4].as<std::int64_t>();
  }
  if (!row[offset + 5].is_null()) {
    result.timed_successor_phase = row[offset + 5].as<int>();
  }
  if (!row[offset + 6].is_null()) {
    result.next_transition_delay_ticks = row[offset + 6].as<std::int64_t>();
  }
  return result;
}

void require_reward_owner_epoch(pqxx::transaction_base &transaction, std::int64_t owner_epoch) {
  if (owner_epoch <= 0) {
    throw std::runtime_error("Reward owner epoch is missing");
  }
  const auto rows = transaction.exec(
      "SELECT epoch FROM emulator_monster_reward_server_owner WHERE singleton FOR SHARE");
  if (rows.size() != 1 || rows.front()[0].as<std::int64_t>() != owner_epoch) {
    throw std::runtime_error("Reward owner epoch is stale or missing");
  }
}

void lock_quest_service_guild(pqxx::work &transaction, std::int64_t guild_id) {
  if (guild_id <= 0) {
    throw std::invalid_argument("Invalid quest service guild guard");
  }
  transaction.exec("INSERT INTO emulator_quest_service_guild_guards(guild_id) VALUES ($1) "
                   "ON CONFLICT DO NOTHING", pqxx::params{guild_id});
  transaction.exec("SELECT guild_id FROM emulator_quest_service_guild_guards "
                   "WHERE guild_id=$1 FOR UPDATE", pqxx::params{guild_id});
}

void lock_quest_service_character(pqxx::work &transaction, std::int64_t character_id) {
  if (character_id <= 0) {
    throw std::invalid_argument("Invalid quest service character guard");
  }
  transaction.exec("INSERT INTO emulator_quest_service_character_guards(character_id) VALUES ($1) "
                   "ON CONFLICT DO NOTHING", pqxx::params{character_id});
  transaction.exec("SELECT character_id FROM emulator_quest_service_character_guards "
                   "WHERE character_id=$1 FOR UPDATE", pqxx::params{character_id});
}

void require_quest_effect_is_first(pqxx::work &transaction, std::int64_t character_id,
                                   const std::string &effect_id) {
  const auto pending = transaction.exec(
      "SELECT effect_id FROM emulator_quest_service_effects WHERE character_id=$1 "
      "AND (NOT company_applied OR NOT guild_applied) ORDER BY enqueue_sequence LIMIT 1",
      pqxx::params{character_id});
  if (pending.empty() || pending.front()[0].as<std::string>() != effect_id) {
    throw std::runtime_error("Quest service effect is not the next pending event");
  }
}

void require_quest_company_write_allowed(pqxx::work &transaction, std::int64_t character_id,
                                         const std::optional<std::string> &settling_effect) {
  const auto pending = transaction.exec(
      "SELECT effect_id FROM emulator_quest_service_effects "
      "WHERE character_id=$1 AND NOT company_applied ORDER BY enqueue_sequence LIMIT 1",
      pqxx::params{character_id});
  if (!pending.empty() &&
      (!settling_effect || pending.front()[0].as<std::string>() != *settling_effect)) {
    throw std::runtime_error("Company update waits for a pending quest reward");
  }
}

void require_quest_guild_write_allowed(pqxx::work &transaction, std::int64_t guild_id,
                                       const std::optional<std::pair<std::int64_t, std::string>>
                                           &settling_effect) {
  const auto pending = transaction.exec(
      "SELECT character_id,effect_id FROM emulator_quest_service_effects "
      "WHERE expected_guild_id=$1 AND NOT guild_applied ORDER BY enqueue_sequence LIMIT 1",
      pqxx::params{guild_id});
  if (!pending.empty() &&
      (!settling_effect || pending.front()[0].as<std::int64_t>() != settling_effect->first ||
       pending.front()[1].as<std::string>() != settling_effect->second)) {
    throw std::runtime_error("Guild update waits for a pending quest reward");
  }
}

void advance_quest_company_generation(pqxx::work &transaction, std::int64_t character_id) {
  const auto changed = transaction.exec(
      "UPDATE emulator_quest_service_character_guards SET company_generation=company_generation+1 "
      "WHERE character_id=$1 AND company_generation < 9223372036854775807 RETURNING character_id",
      pqxx::params{character_id});
  if (changed.empty()) {
    throw std::runtime_error("Company membership generation exhausted or guard missing");
  }
}

void advance_quest_guild_generation(pqxx::work &transaction, std::int64_t character_id) {
  const auto changed = transaction.exec(
      "UPDATE emulator_quest_service_character_guards SET guild_generation=guild_generation+1 "
      "WHERE character_id=$1 AND guild_generation < 9223372036854775807 RETURNING character_id",
      pqxx::params{character_id});
  if (changed.empty()) {
    throw std::runtime_error("Guild membership generation exhausted or guard missing");
  }
}

} // namespace chronicles::detail
