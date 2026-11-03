#pragma once

#include "persistence/character_state_types.hpp"

#include <pqxx/pqxx>

#include <cstddef>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace chronicles::detail {

// Caller holds all source-row locks before reserving per-world creation capacity.
bool reserve_shared_source_capacity(pqxx::work &transaction, int world_id,
                                    std::size_t amount = 1);
StoredSharedGatheringSource decode_shared_gathering_source(const pqxx::row &row,
                                                           std::size_t offset = 0);

void require_reward_owner_epoch(pqxx::transaction_base &transaction, std::int64_t owner_epoch);

// Transaction lock order: guild guard, character guard, effect row, then entity rows.
// A caller that discovers a guild from membership must re-read it after acquiring both guards.
void lock_quest_service_guild(pqxx::work &transaction, std::int64_t guild_id);
void lock_quest_service_character(pqxx::work &transaction, std::int64_t character_id);
void require_quest_effect_is_first(pqxx::work &transaction, std::int64_t character_id,
                                   const std::string &effect_id);
void require_quest_company_write_allowed(pqxx::work &transaction, std::int64_t character_id,
                                         const std::optional<std::string> &settling_effect = {});
void require_quest_guild_write_allowed(pqxx::work &transaction, std::int64_t guild_id,
                                       const std::optional<std::pair<std::int64_t, std::string>>
                                           &settling_effect = {});
void advance_quest_company_generation(pqxx::work &transaction, std::int64_t character_id);
void advance_quest_guild_generation(pqxx::work &transaction, std::int64_t character_id);

} // namespace chronicles::detail
