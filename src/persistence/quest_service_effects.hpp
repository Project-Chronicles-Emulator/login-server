#pragma once

#include "persistence/character_state_types.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace chronicles {

inline constexpr std::size_t kQuestServiceEffectPageSize = 128;
inline constexpr std::size_t kMaximumPendingQuestServiceEffectsPerCharacter = 1024;
inline constexpr std::size_t kMaximumStoredQuestServiceEvents = 1000000;

enum class QuestServiceComponent { company, guild };

struct QuestServiceUnlock {
  int company_type{};
  int level{};

  bool operator==(const QuestServiceUnlock &) const = default;
};

// Membership identities are captured for the quest transition. Zero means the character was
// not a member, rather than permitting a later membership to receive the reward.
struct QuestServiceEffects {
  std::vector<QuestServiceUnlock> company_unlocks;
  std::int64_t company_contribution{};
  std::int64_t guild_experience{};
  // Actual player EXP awarded by the same committed mutation, with conversion rates
  // pinned at capture so delayed settlement does not use a later configuration.
  std::int64_t activity_experience{};
  std::int64_t activity_company_rate{};
  std::int64_t activity_guild_rate{};
  int expected_company_type{};
  std::int64_t expected_guild_id{};
  std::int64_t expected_company_generation{};
  std::int64_t expected_guild_generation{};

  bool operator==(const QuestServiceEffects &) const = default;
};

struct QuestServiceMembership {
  int company_type{};
  std::int64_t guild_id{};
  std::int64_t company_generation{};
  std::int64_t guild_generation{};

  bool operator==(const QuestServiceMembership &) const = default;
};

struct QuestServiceEffectEvent {
  std::uint64_t enqueue_sequence{};
  std::int64_t account_id{};
  std::int64_t character_id{};
  int world_id{};
  std::string effect_id;
  std::string mutation_fingerprint;
  bool monster_receipt{};
  QuestServiceEffects effects;
  std::vector<CharacterItem> granted_items;
  bool company_applied{};
  bool guild_applied{};
};

enum class QuestMutationStatus { committed, already_processed, conflict };

struct QuestMutationResult {
  QuestMutationStatus status{QuestMutationStatus::conflict};
  std::vector<CharacterItem> granted_items;
};

// Versioned, deterministic input identity for an uncertain quest transaction. The result is
// persisted with the operation ID; replay must compare the entire value before using a tombstone.
nlohmann::json canonical_quest_mutation(const WorldProgress &before,
                                         const std::optional<WorldProgress> &after,
                                         const ItemTrade &trade,
                                         const QuestServiceEffects &effects);

std::string quest_mutation_fingerprint(const WorldProgress &before,
                                        const std::optional<WorldProgress> &after,
                                        const ItemTrade &trade,
                                        const QuestServiceEffects &effects);

// Binds the retained combat batch and output to its exact quest mutation.
std::string combat_quest_source_fingerprint(const nlohmann::json &source);

std::string combat_quest_event_fingerprint(const std::string &source_fingerprint,
                                          const WorldProgress &before,
                                          const std::optional<WorldProgress> &after,
                                          const ItemTrade &trade,
                                          const QuestServiceEffects &effects);

std::string gathering_mutation_fingerprint(
    const std::string &source_fingerprint, const WorldProgress &before,
    const WorldProgress &after, const ItemTrade &trade, const QuestServiceEffects &effects,
    const std::optional<SharedGatheringSourceTransition> &source);

void validate_shared_gathering_source(const SharedGatheringSourceTransition &source);

} // namespace chronicles
