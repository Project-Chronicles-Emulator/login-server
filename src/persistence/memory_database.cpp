// Shared helpers of the in-memory repositories; every function requires the caller to hold
// MemoryDatabase::mutex (see memory_database.hpp).

#include "persistence/memory_database.hpp"

#include <algorithm>
#include <utility>

namespace chronicles::detail {

std::vector<Character>::iterator
find_owned_character(MemoryDatabase &database, std::int64_t account_id,
                     std::int64_t character_id, int world_id) {
  return std::ranges::find_if(database.characters, [&](const Character &character) {
    return character.id == character_id && character.account_id == account_id &&
           character.world_id == world_id;
  });
}

void commit_progress_and_items(MemoryDatabase &database, Character &owner,
                               const WorldProgress &after, std::vector<CharacterItem> items,
                               std::int64_t next_item_id) {
  database.world_progress[owner.id] = after;
  owner.items = std::move(items);
  database.next_item_id = next_item_id;
}

} // namespace chronicles::detail
