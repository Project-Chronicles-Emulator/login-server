#pragma once

// PostgreSQL character-row helpers shared by the persistence repositories and the feature
// stores that commit progress transitions in the same transaction.

#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"

#include <pqxx/pqxx>

#include <cstdint>
#include <vector>

namespace chronicles::detail {

// Throws std::invalid_argument when `after` is out of range or is not exactly one revision
// after `before` with every existing row preserved.
void validate_world_progress_transition(const WorldProgress &before, const WorldProgress &after);

// Applies a validated transition inside the caller's transaction. Returns false when the stored
// revision or ownership does not match; the caller then abandons the transaction.
bool write_world_progress_transition(pqxx::work &transaction, std::int64_t account_id,
                                     std::int64_t character_id, int world_id,
                                     const WorldProgress &before, const WorldProgress &after);

// Reads one character's persisted build (the six attribute bonus columns and its perk rows).
// Character and world-progress loads both use it, so the two copies cannot diverge.
CharacterBuild read_character_build(pqxx::transaction_base &transaction,
                                    std::int64_t character_id);

// Rewrites the owned item rows inside the caller's transaction, vacating equipped slots before
// filling them to honor the partial unique index. Returns false when an update matched no row;
// the caller then abandons the transaction.
bool update_items_in_transaction(pqxx::work &transaction, std::int64_t account_id,
                                 std::int64_t character_id, int world_id,
                                 const std::vector<CharacterItem> &items);

} // namespace chronicles::detail
