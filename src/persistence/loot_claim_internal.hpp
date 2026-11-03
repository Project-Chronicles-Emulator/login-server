#pragma once

// Shared trade primitives; callers hold the memory mutex or own the PostgreSQL transaction.
#include "persistence/character_state_types.hpp"
#include "persistence/character_repository.hpp"

#include <pqxx/pqxx>

#include <cstdint>
#include <optional>
#include <vector>

namespace chronicles {

struct MemoryDatabase;

namespace detail {

void validate_loot_trade(const WorldProgress &before, const WorldProgress &after,
                         const ItemTrade &trade);

// Caller holds the character row lock for the entire transaction.
WorldProgress read_locked_world_progress(pqxx::transaction_base &transaction,
                                          std::int64_t account_id,
                                          std::int64_t character_id, int world_id);

struct PreparedMemoryLootTrade {
  Character owner;
  WorldProgress progress;
  std::vector<CharacterItem> granted_items;
  std::int64_t next_item_id{};
};

// These helpers perform no mutation until publish. The caller holds the database mutex,
// validates ownership/revision, and has created the existing progress row before publication.
std::optional<PreparedMemoryLootTrade>
prepare_memory_loot_trade(const MemoryDatabase &database, const Character &owner,
                          const WorldProgress &after, const ItemTrade &trade);
void publish_memory_loot_trade(MemoryDatabase &database, Character &owner,
                               WorldProgress &stored_progress,
                               PreparedMemoryLootTrade &prepared) noexcept;

std::optional<std::vector<CharacterItem>>
apply_postgres_loot_trade(pqxx::work &transaction, std::int64_t account_id,
                          std::int64_t character_id, int world_id,
                          const WorldProgress &before, const WorldProgress &after,
                          const ItemTrade &trade);

} // namespace detail
} // namespace chronicles
