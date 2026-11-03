#pragma once

#include "persistence/character_state_types.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles {

enum class CharacterRenameResult {
  renamed,
  not_found,
  name_used,
};

struct Character {
  std::int64_t id{};
  std::int64_t account_id{};
  int world_id{1};
  int slot{};
  int class_id{1};
  int level{1};
  std::string name;
  std::string customization_hex;
  std::optional<WorldPosition> world_position;
  std::int64_t experience{};
  std::vector<CharacterItem> items;
  CharacterBuild build;
  int nui_blessing_level{};
  int dedicated_nui_tears{};
};

// Display identity of a stored character for social features (friend lists and search).
// Loaded without items, progress or customization, so lookups stay cheap.
struct CharacterSummary {
  std::int64_t id{};
  std::int64_t account_id{};
  int world_id{};
  int slot{};
  int class_id{1};
  int level{1};
  std::string name;

  bool operator==(const CharacterSummary &) const = default;
};

enum class CreateFailure { slot_full, name_used };

class CreateError : public std::runtime_error {
public:
  explicit CreateError(CreateFailure failure_reason)
      : std::runtime_error("Character conflict"), reason(failure_reason) {}

  CreateFailure reason;
};

// One-time migration planner. Existing CMS items and equipped slots are never replaced.
std::vector<CharacterItem> missing_starter_items(const Character &character);

// Shared preconditions for unversioned character edits. These perform no storage I/O.
void validate_world_position(const WorldPosition &position);
void validate_progress(int level, std::int64_t experience);

// Character rows: lobby and operator listings, creation, rename, deletion, social summaries
// and the unguarded world-position and level/experience edits.
// Implementations: MemoryCharacterRepository and PostgresCharacterRepository.
class CharacterRepository {
public:
  virtual ~CharacterRepository() = default;

  virtual std::vector<Character> characters(std::int64_t account_id, int world_id) = 0;

  virtual Character create(Character character) = 0;

  virtual bool save_world_position(std::int64_t account_id, std::int64_t character_id, int world_id,
                                   const WorldPosition &position) = 0;

  virtual bool save_progress(std::int64_t account_id, std::int64_t character_id, int world_id,
                             int level, std::int64_t experience) = 0;

  // Operator console operations (src/persistence/admin_storage.cpp). Only the authenticated
  // admin API calls these; no game-client request reaches them.
  // Every character of every account, without item rows.
  virtual std::vector<Character> list_characters() = 0;

  // One character with its item rows.
  virtual std::optional<Character> character_by_id(std::int64_t character_id) = 0;

  virtual CharacterRenameResult rename_character(std::int64_t character_id,
                                                 const std::string &name) = 0;

  // Removes the character and every row it owns (items, progress, quests).
  virtual bool delete_character(std::int64_t character_id) = 0;

  // Social lookups (src/persistence/character_summary_storage.cpp). Summaries omit items and
  // progress.
  virtual std::optional<CharacterSummary> character_summary(std::int64_t character_id) = 0;

  // Exact, case-sensitive name match on one realm.
  virtual std::optional<CharacterSummary> character_summary_by_name(int world_id,
                                                                    const std::string &name) = 0;

  // The account's characters on one realm, ordered by lobby slot.
  virtual std::vector<CharacterSummary> account_character_summaries(std::int64_t account_id,
                                                                    int world_id) = 0;
};

} // namespace chronicles
