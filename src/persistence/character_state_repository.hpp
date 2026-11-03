#pragma once

#include "persistence/skill_item_use.hpp"

#include "persistence/character_state_types.hpp"
#include "persistence/quest_service_effects.hpp"

#include <cstdint>
#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles {

// Callers drain one ordered page at a time. A full queue refuses new defeats rather than
// dropping pending credit; retry must preserve the original event and recipient batch.
inline constexpr std::size_t kMonsterRewardPageSize = 128;
inline constexpr std::size_t kMonsterRewardRecoveryPageSize = 128;
inline constexpr std::size_t kMaximumPendingMonsterRewardsPerCharacter = 1024;
inline constexpr std::size_t kMaximumStoredMonsterRewardEvents = 1000000;

// Revisioned character state: item rows and world-progress transitions, committed together by
// the composite operations under gameplay_revision.
// Implementations: MemoryCharacterStateRepository and PostgresCharacterStateRepository.
class CharacterStateRepository {
public:
  virtual ~CharacterStateRepository() = default;

  // These primitives are inactive until admission retains and recovers the combat continuation.
  // The caller retains expected_sequence before the first reservation attempt. Lookup exposes
  // terminal receipts too, so the next sequence can be chosen without resetting on reconnect.
  virtual SkillItemUseResult reserve_skill_item_use(
      std::int64_t, std::int64_t, int, std::int64_t, const SkillItemUsePlan &) {
    throw std::runtime_error("Skill item use reservation is unavailable");
  }
  virtual SkillItemUseResult commit_skill_item_use(
      std::int64_t, std::int64_t, int, std::int64_t, const SkillItemUsePlan &) {
    throw std::runtime_error("Skill item use commit is unavailable");
  }
  virtual std::optional<SkillItemUseReceipt> lookup_skill_item_use(
      std::int64_t, std::int64_t, int) {
    throw std::runtime_error("Skill item use lookup is unavailable");
  }
  virtual bool finish_skill_item_use(std::int64_t, std::int64_t, int, std::int64_t,
                                     const std::string &, bool) {
    throw std::runtime_error("Skill item use settlement is unavailable");
  }

  // The zero-valued membership identities mean no membership, not a wildcard. The
  // generations change on every membership transition, including leave/rejoin to the same ID.
  virtual std::optional<QuestServiceMembership> read_quest_service_membership(
      std::int64_t, std::int64_t, int) {
    throw std::runtime_error("Quest service membership capture is unavailable");
  }

  // Unlike an effect receipt this operation is not replayable by ID. A caller that loses
  // its acknowledgement must fence the progress revision and reconnect before retrying.
  virtual std::optional<std::vector<CharacterItem>> commit_unaffiliated_activity(
      std::int64_t, std::int64_t, int, const QuestServiceMembership &,
      const WorldProgress &, const WorldProgress &, const ItemTrade &) {
    throw std::runtime_error("Unaffiliated activity commit is unavailable");
  }

  // Commits an immutable gathering source claim, the combined progress/items and any
  // quest service effects together. Exceptions indicate an unknown COMMIT outcome;
  // callers must retain and retry the exact same receipt and payload.
  virtual LootClaimResult commit_gathering_quest_claim(
      std::int64_t, std::int64_t, int, const std::string &, const std::string &,
      const WorldProgress &, const WorldProgress &, const ItemTrade &,
      const QuestServiceEffects &, const std::optional<SharedGatheringSourceTransition> &) {
    throw std::runtime_error("Durable gathering quest claim is unavailable");
  }

  // Idempotently arms the shared-source cooldown from the first confirmed completion
  // time and marks the receipt terminal. A retry returns the original armed deadline.
  virtual GatheringFinalizeResult finalize_gathering_quest_claim(
      std::int64_t, std::int64_t, int, const std::string &, const std::string &,
      const WorldProgress &, const WorldProgress &, const ItemTrade &,
      const QuestServiceEffects &, const std::optional<SharedGatheringSourceTransition> &,
      std::int64_t) {
    throw std::runtime_error("Durable gathering claim finalization is unavailable");
  }

  // Startup only, while the exclusive publisher lease is held and before admission.
  // Each bounded page arms committed source cooldowns and retires their receipts.
  virtual std::size_t recover_gathering_claims(std::size_t, std::int64_t) {
    throw std::runtime_error("Gathering claim startup recovery is unavailable");
  }

  virtual bool has_unfinished_gathering_claims() {
    throw std::runtime_error("Gathering claim startup recovery is unavailable");
  }

  virtual std::vector<SharedGatheringSourceRecord> shared_gathering_sources(
      int, const std::optional<SharedGatheringSourceKey> &, std::size_t) {
    throw std::runtime_error("Shared gathering source hydration is unavailable");
  }

  virtual SharedGatheringSourceUpdateResult update_shared_gathering_source(
      int, const SharedGatheringSourceTransition &, std::int64_t, bool) {
    throw std::runtime_error("Shared gathering source transition is unavailable");
  }

  // Bump the authoritative generation without changing its phase or deadline. An absent
  // source may only be initialized from a verified authored initial state (generation zero).
  // A pending receipt is returned without mutation and must remain quarantined.
  virtual SharedGatheringSourceUpdateResult fence_shared_gathering_source(
      int, const SharedGatheringSourceTransition &) {
    throw std::runtime_error("Shared gathering source fencing is unavailable");
  }

  // Returns the oldest unresolved event per character in enqueue order. The cursor is
  // applied only after selecting each character's oldest event, preserving FIFO on wrap.
  virtual std::vector<QuestServiceEffectEvent> pending_quest_service_effects(
      int, const std::optional<std::int64_t> &, std::size_t, std::uint64_t = 0) {
    throw std::runtime_error("Quest service effect recovery is unavailable");
  }

  // Commits one quest transition and its service effects as one idempotent operation.
  // Implementations must check both membership identities against the serialized state.
  virtual QuestMutationResult commit_quest_mutation(
      std::int64_t, std::int64_t, int, const std::string &, const WorldProgress &,
      const std::optional<WorldProgress> &, const ItemTrade &, const QuestServiceEffects &) {
    throw std::runtime_error("Durable quest service effects are unavailable");
  }

  // The combat owner must retain the exact batch and output through acknowledgment.
  // A null after records an idempotent no-op at the captured revision.
  virtual QuestMutationResult commit_combat_quest_event(
      std::int64_t, std::int64_t, int, const std::string &, const std::string &,
      const WorldProgress &, const std::optional<WorldProgress> &, const ItemTrade &,
      const QuestServiceEffects &) {
    throw std::runtime_error("Durable combat quest event is unavailable");
  }

  // The recipient's monster receipt supplies the stable effect identity.
  virtual MonsterRewardCommitResult commit_monster_reward_with_effects(
      std::int64_t, std::int64_t, int, const std::string &, const WorldProgress &,
      const std::optional<WorldProgress> &, const ItemTrade &, const QuestServiceEffects &) {
    throw std::runtime_error("Durable monster quest service effects are unavailable");
  }

  // Durable write-ahead fence for unversioned offline operator mutations. Implementations
  // that cannot provide it must fail closed; marker tokens are opaque random identifiers.
  virtual std::vector<std::int64_t> offline_admin_quarantines() {
    throw std::runtime_error("Offline admin quarantine is unavailable");
  }
  virtual bool create_offline_admin_quarantine(std::int64_t, const std::string &) {
    throw std::runtime_error("Offline admin quarantine is unavailable");
  }
  virtual bool clear_offline_admin_quarantine(std::int64_t, const std::string &) {
    throw std::runtime_error("Offline admin quarantine is unavailable");
  }

  virtual std::optional<CharacterItem> grant_item(std::int64_t account_id,
                                                  std::int64_t character_id, int world_id,
                                                  CharacterItem item) = 0;

  virtual bool update_item(std::int64_t account_id, std::int64_t character_id, int world_id,
                           const CharacterItem &item) = 0;

  // Applies item swaps atomically under the character revision. Vacate
  // equipped slots before filling them to honor slot uniqueness.
  virtual bool update_items(std::int64_t account_id, std::int64_t character_id, int world_id,
                            std::int64_t expected_revision,
                            const std::vector<CharacterItem> &items) = 0;

  // These transitions each advance gameplay_revision exactly once on success.
  virtual bool commit_world_death(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      std::int64_t expected_revision,
      const std::vector<EquippedDurabilityChange> &durability_changes) = 0;

  virtual bool commit_world_revival(std::int64_t account_id, std::int64_t character_id,
                                    int world_id, std::int64_t expected_revision,
                                    const WorldPosition &position) = 0;

  // Commits a dead-to-alive transition, supplied progress and destination together.
  virtual bool commit_world_shrine_revival(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      const WorldProgress &before, const WorldProgress &after,
      const WorldPosition &position) = 0;

  // Atomically discard owned unequipped instances and decrement stackables
  // under the same character revision used by world progress.
  virtual bool discard_items(std::int64_t account_id, std::int64_t character_id, int world_id,
                             std::int64_t expected_revision,
                             const std::vector<std::int64_t> &item_ids,
                             const std::map<int, int> &stackable_counts) = 0;

  // Commits one tempering attempt atomically under the character revision: the item change
  // selected by change.outcome, the material and additive stackables and gold change together
  // or not at all. Returns false (changing nothing) for a stale revision, an unknown item, a
  // polish_step that is not the expected one, insufficient material, additive or gold, or a
  // destruction of an equipped item.
  virtual bool apply_item_polish(std::int64_t account_id, std::int64_t character_id, int world_id,
                                 std::int64_t expected_revision,
                                 const ItemPolishChange &change) = 0;

  virtual bool remove_item(std::int64_t account_id, std::int64_t character_id, int world_id,
                           std::int64_t item_id) = 0;

  virtual WorldProgress load_world_progress(std::int64_t account_id, std::int64_t character_id,
                                            int world_id) = 0;

  // Compare revisions and commit all changed rewards/object/quest rows together.
  virtual bool save_world_progress(std::int64_t account_id, std::int64_t character_id, int world_id,
                                   const WorldProgress &before, const WorldProgress &after) = 0;

  // Fence delayed progress transactions before reading a new world session.
  virtual bool fence_world_entry(std::int64_t account_id, std::int64_t character_id,
                                 int world_id) = 0;

  // Atomically advance the owned character revision and return its progress and equipped
  // items from the same snapshot. nullopt means no owned row or exhausted revision; an
  // exception has an unknown fence outcome, so the caller must retry the fence before acting.
  virtual std::optional<DeathReconciliationSnapshot> fence_world_death_reconciliation(
      std::int64_t account_id, std::int64_t character_id, int world_id) {
    throw std::runtime_error("Death reconciliation is unavailable");
  }

  // Commits a revisioned progress update and the character's world position together.
  virtual bool commit_world_progress_and_position(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      const WorldProgress &before, const WorldProgress &after, const WorldPosition &position) = 0;

  // Commits a revisioned world-progress transition together with the trade's item changes in
  // one transaction. Granted items receive storage IDs in the returned copy. Returns nullopt,
  // leaving every row unchanged, when the revision, ownership, an equipped removal, a stale
  // durability, or an inventory slot conflicts.
  virtual std::optional<std::vector<CharacterItem>>
  commit_item_trade(std::int64_t account_id, std::int64_t character_id, int world_id,
                    const WorldProgress &before, const WorldProgress &after,
                    const ItemTrade &trade) = 0;

  // Read a receipt before replanning: a previous committed grant may have filled the bag.
  virtual LootClaimResult lookup_loot_claim(std::int64_t account_id, std::int64_t character_id,
                                            int world_id, const std::string &receipt_id,
                                            const std::string &corpse_fingerprint) {
    throw std::runtime_error("Durable loot claims are unavailable");
  }

  // Atomically award one corpse and persist its immutable identity. Repeating a committed
  // receipt never applies the transition again, even when the first COMMIT acknowledgement
  // was lost. A different claimant or corpse fingerprint can never replace that receipt.
  virtual LootClaimResult commit_loot_claim(std::int64_t account_id, std::int64_t character_id,
                                            int world_id, const std::string &receipt_id,
                                            const std::string &corpse_fingerprint,
                                            const WorldProgress &before,
                                            const WorldProgress &after, const ItemTrade &trade) {
    throw std::runtime_error("Durable loot claims are unavailable");
  }

  // Insert all recipients of one defeat in one transaction. A replay is accepted only if every
  // existing receipt has the same payload; an ownership or payload conflict writes nothing.
  virtual bool enqueue_monster_rewards(const std::vector<MonsterRewardReceipt> &receipts) = 0;

  // A script intent and all recipient snapshots form one immutable defeat event.
  // Empty recipients are valid only when an allowlisted boss intent is supplied.
  virtual bool enqueue_monster_rewards_with_script(
      const std::vector<MonsterRewardReceipt> &, const std::optional<BossScriptIntent> &) {
    throw std::runtime_error("Durable boss script enqueue is unavailable");
  }
  virtual std::vector<BossScriptIntent> pending_boss_scripts(
      int, const std::optional<std::string> &, std::size_t) {
    throw std::runtime_error("Durable boss script recovery is unavailable");
  }
  virtual BossScriptSettlement settle_shared_boss_script(const BossScriptIntent &) {
    throw std::runtime_error("Durable boss script settlement is unavailable");
  }
  // Called only after current source snapshots are installed and ordered delivery is queued.
  virtual bool mark_boss_script_published(const BossScriptIntent &) {
    throw std::runtime_error("Boss script publication acknowledgement is unavailable");
  }
  // A live private copy acknowledges only after its local script is installed.
  virtual bool mark_private_boss_script_applied(const BossScriptIntent &) {
    throw std::runtime_error("Private boss script acknowledgement is unavailable");
  }
  // Only startup under the exclusive owner lease may retire an old private-copy intent.
  virtual bool retire_private_boss_script(const BossScriptIntent &) {
    throw std::runtime_error("Private boss script retirement is unavailable");
  }

  // Startup calls this only after taking the process-lifetime exclusive database lease. The
  // epoch update waits for older reward transactions, then fences all stale owners.
  virtual std::int64_t acquire_monster_reward_owner_epoch() = 0;
  virtual void adopt_monster_reward_owner_epoch(std::int64_t epoch) = 0;

  // A staged event is invisible to consumers until the encounter has finalized. Idempotent;
  // an exception may follow a committed update, so the caller retries the same event ID.
  virtual bool mark_monster_reward_ready(const std::string &receipt_id) = 0;

  // Startup only, while the caller holds exclusive server authority and before admissions.
  // Marks at most `limit` staged events ready. A database error must abort startup.
  virtual std::size_t recover_staged_monster_rewards(std::size_t limit) = 0;
  virtual bool has_staged_monster_rewards() = 0;

  virtual std::vector<MonsterRewardReceipt>
  pending_monster_rewards(std::int64_t account_id, std::int64_t character_id, int world_id) = 0;

  // The receipt and the optional progress/item transition commit atomically. When no change is
  // required, `after` is empty and the receipt is marked processed only at the expected revision.
  // An already processed receipt is never awarded twice, including after a lost acknowledgement.
  virtual MonsterRewardCommitResult commit_monster_reward(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      const std::string &receipt_id,
      const WorldProgress &before, const std::optional<WorldProgress> &after,
      const ItemTrade &trade) = 0;
};

} // namespace chronicles
