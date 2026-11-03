#pragma once

#include "persistence/account_repository.hpp"
#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"
#include "persistence/memory_quest_service_coordinator.hpp"
#include "persistence/skill_item_use.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chronicles {

// In-memory tables shared by every memory repository. One mutex guards all of them so
// operations that span data areas (market and mail commits, trades, deletion) stay atomic.
// Every member is read or written only while `mutex` is held.
struct MemoryDatabase {
  struct ReconnectCredential {
    std::int64_t account_id{};
    std::int64_t expires_at{};
  };

  struct StoredMonsterReward {
    MonsterRewardReceipt receipt;
    bool processed{};
    std::uint64_t enqueue_sequence{};
  };

  struct StoredMonsterRewardBatch {
    std::vector<MonsterRewardReceipt> recipients;
    bool ready{};
    std::optional<BossScriptIntent> boss_script;
    bool boss_script_applied{true};
    bool boss_script_published{true};
  };

  // Lets a feature that keeps per-character rows block or follow a character deletion.
  // Both callbacks run with `mutex` held and must not lock it again.
  struct CharacterDeletionHook {
    std::function<bool(std::int64_t character_id)> blocks_deletion;
    std::function<void(std::int64_t character_id)> erase_rows;
  };

  std::mutex mutex;
  std::shared_ptr<MemoryQuestServiceCoordinator> quest_services =
      std::make_shared<MemoryQuestServiceCoordinator>();
  std::vector<Account> accounts;
  std::unordered_map<std::string, ReconnectCredential> reconnect_credentials;
  std::unordered_map<std::string, std::int64_t> discord_accounts;
  std::vector<Character> characters;
  std::int64_t next_character_id{1};
  std::int64_t next_item_id{1};
  std::unordered_map<std::int64_t, WorldProgress> world_progress;
  std::unordered_map<std::int64_t, std::string> offline_admin_quarantines;
  std::map<std::string, StoredMonsterRewardBatch> monster_reward_batches;
  // Only ready, unpublished script IDs are indexed; retained tombstones are not scanned.
  std::map<int, std::set<std::string>> pending_boss_script_ids;
  std::map<int, std::size_t> monster_reward_event_counts;
  std::map<std::pair<std::int64_t, std::string>, StoredMonsterReward> monster_rewards;
  std::map<std::string, StoredLootClaim> loot_claims;
  std::map<std::int64_t, SkillItemUseReceipt> skill_item_uses;
  std::map<int, std::size_t> loot_claim_counts;
  std::map<int, std::size_t> gathering_source_counts;
  std::map<std::tuple<int, int, int, std::uint32_t, std::uint32_t>,
           StoredSharedGatheringSource> gathering_sources;
  std::uint64_t next_monster_reward_sequence{1};
  std::int64_t monster_reward_owner_epoch{};
  std::vector<CharacterDeletionHook> deletion_hooks;
};

namespace detail {

// Caller holds database.mutex. Finds the character row owned by the account on this world, or
// database.characters.end() when there is none. Shared by the mail claim, item-trade and
// company-permit commits.
std::vector<Character>::iterator
find_owned_character(MemoryDatabase &database, std::int64_t account_id,
                     std::int64_t character_id, int world_id);

// Caller holds database.mutex. Writes the validated progress value, replaces the owner's item
// rows and publishes the item ids reserved while the caller planned the change. Shared by the
// mail claim, market and item-trade commits; callers that mirror level, experience and build
// into the character row do that separately.
void commit_progress_and_items(MemoryDatabase &database, Character &owner,
                               const WorldProgress &after, std::vector<CharacterItem> items,
                               std::int64_t next_item_id);

} // namespace detail

} // namespace chronicles
