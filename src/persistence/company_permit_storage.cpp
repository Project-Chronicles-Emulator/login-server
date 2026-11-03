#include "persistence/company_record.hpp"
#include "persistence/memory_repositories.hpp"
#include "persistence/postgres/character_rows.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace chronicles {

bool MemoryCharacterStateRepository::commit_company_permit(
    std::int64_t account_id, int world_id, CompanyRecord &company,
    const CompanyRecord &next_company, const WorldProgress &before, const WorldProgress &after) {
  detail::validate_world_progress_transition(before, after);
  static_assert(std::is_nothrow_copy_assignable_v<CompanyRecord>);

  // A permit can only change currency and the revision. Validate before touching storage;
  // those scalar assignments and the company record assignment cannot throw.
  auto expected = before;
  expected.gold = after.gold;
  expected.hersander_token = after.hersander_token;
  expected.revision = after.revision;
  if (expected != after) {
    throw std::invalid_argument("Permit transition changed non-currency progress");
  }

  std::lock_guard lock(database_->mutex);
  const auto character =
      detail::find_owned_character(*database_, account_id, company.char_id, world_id);
  if (character == database_->characters.end()) {
    return false;
  }
  auto &stored = database_->world_progress[company.char_id];
  if (stored.revision != before.revision || stored.dead != before.dead) {
    return false;
  }

  stored.gold = after.gold;
  stored.hersander_token = after.hersander_token;
  stored.revision = after.revision;
  company = next_company;
  return true;
}

} // namespace chronicles
