#pragma once

#include "persistence/account_repository.hpp"
#include "persistence/character_repository.hpp"
#include "persistence/character_state_repository.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace chronicles {

// PostgreSQL implementation of AccountRepository. The repository keeps its own copy of the
// connection string and opens one pqxx::connection per call; transactions and row locks
// provide the atomicity, so there is no shared state to guard.
class PostgresAccountRepository final : public AccountRepository {
public:
  explicit PostgresAccountRepository(std::string connection)
      : connection_(std::move(connection)) {}

  std::optional<Account> account(const std::string &name) override;

  bool add_account(Account account) override;

  std::optional<Account> discord_account(const std::string &discord_id) override;
  Account create_discord_account(const std::string &discord_id, Account candidate) override;
  bool link_discord_account(const std::string &discord_id, std::int64_t account_id) override;

  bool set_account_role(const std::string &name, AccountRole role) override;

  std::vector<AccountSummary> list_accounts() override;

  std::optional<Account> account_by_id(std::int64_t account_id) override;

  bool save_reconnect_credential(std::int64_t account_id, const std::string &digest,
                                 std::int64_t expires_at) override;

  std::optional<Account> reconnect_account(const std::string &digest,
                                           std::int64_t now) override;

  void revoke_reconnect_credentials(std::int64_t account_id) override;

  bool set_account_password(std::int64_t account_id, const std::string &salt,
                            const std::string &hash, int iterations) override;

  bool set_account_banned(std::int64_t account_id, bool banned) override;

private:
  std::string connection_;
};

// PostgreSQL implementation of CharacterRepository; ownership and connection handling follow
// PostgresAccountRepository.
class PostgresCharacterRepository final : public CharacterRepository {
public:
  explicit PostgresCharacterRepository(std::string connection)
      : connection_(std::move(connection)) {}

  std::vector<Character> characters(std::int64_t account_id, int world_id) override;

  Character create(Character character) override;

  bool save_world_position(std::int64_t account_id, std::int64_t character_id, int world_id,
                           const WorldPosition &position) override;

  bool save_progress(std::int64_t account_id, std::int64_t character_id, int world_id,
                     int level, std::int64_t experience) override;

  std::vector<Character> list_characters() override;

  std::optional<Character> character_by_id(std::int64_t character_id) override;

  CharacterRenameResult rename_character(std::int64_t character_id,
                                         const std::string &name) override;

  bool delete_character(std::int64_t character_id) override;

  std::optional<CharacterSummary> character_summary(std::int64_t character_id) override;

  std::optional<CharacterSummary> character_summary_by_name(int world_id,
                                                          const std::string &name) override;

  std::vector<CharacterSummary> account_character_summaries(std::int64_t account_id,
                                                            int world_id) override;

private:
  std::string connection_;
};

// PostgreSQL implementation of CharacterStateRepository; ownership and connection handling
// follow PostgresAccountRepository.
class PostgresCharacterStateRepository final : public CharacterStateRepository {
public:
  SkillItemUseResult reserve_skill_item_use(std::int64_t, std::int64_t, int, std::int64_t,
                                            const SkillItemUsePlan &) override;
  SkillItemUseResult commit_skill_item_use(std::int64_t, std::int64_t, int, std::int64_t,
                                           const SkillItemUsePlan &) override;
  std::optional<SkillItemUseReceipt> lookup_skill_item_use(std::int64_t, std::int64_t,
                                                           int) override;
  bool finish_skill_item_use(std::int64_t, std::int64_t, int, std::int64_t,
                             const std::string &, bool) override;
  LootClaimResult commit_gathering_quest_claim(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      const std::string &receipt_id, const std::string &source_fingerprint,
      const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
      const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source) override;
  GatheringFinalizeResult finalize_gathering_quest_claim(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      const std::string &receipt_id, const std::string &source_fingerprint,
      const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
      const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source,
      std::int64_t confirmation_epoch_ticks) override;
  std::size_t recover_gathering_claims(std::size_t limit,
                                       std::int64_t confirmation_epoch_ticks) override;
  bool has_unfinished_gathering_claims() override;
  std::vector<SharedGatheringSourceRecord> shared_gathering_sources(
      int world_id, const std::optional<SharedGatheringSourceKey> &after,
      std::size_t limit) override;
  SharedGatheringSourceUpdateResult update_shared_gathering_source(
      int world_id, const SharedGatheringSourceTransition &transition,
      std::int64_t epoch_ticks, bool require_due) override;
  SharedGatheringSourceUpdateResult fence_shared_gathering_source(
      int world_id, const SharedGatheringSourceTransition &initial) override;
  std::optional<std::vector<CharacterItem>> commit_unaffiliated_activity(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      const QuestServiceMembership &membership, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade) override;
  explicit PostgresCharacterStateRepository(std::string connection)
      : connection_(std::move(connection)) {}

  std::optional<QuestServiceMembership> read_quest_service_membership(
      std::int64_t account_id, std::int64_t character_id, int world_id) override;
  std::vector<QuestServiceEffectEvent> pending_quest_service_effects(
      int world_id, const std::optional<std::int64_t> &character_id,
      std::size_t limit, std::uint64_t after_sequence = 0) override;
  QuestMutationResult commit_quest_mutation(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      const std::string &effect_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade,
      const QuestServiceEffects &effects) override;
  QuestMutationResult commit_combat_quest_event(
      std::int64_t, std::int64_t, int, const std::string &, const std::string &,
      const WorldProgress &, const std::optional<WorldProgress> &, const ItemTrade &,
      const QuestServiceEffects &) override;
  MonsterRewardCommitResult commit_monster_reward_with_effects(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      const std::string &receipt_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade,
      const QuestServiceEffects &effects) override;

  LootClaimResult lookup_loot_claim(std::int64_t account_id, std::int64_t character_id,
                                    int world_id, const std::string &receipt_id,
                                    const std::string &corpse_fingerprint) override;
  LootClaimResult commit_loot_claim(std::int64_t account_id, std::int64_t character_id,
                                    int world_id, const std::string &receipt_id,
                                    const std::string &corpse_fingerprint,
                                    const WorldProgress &before, const WorldProgress &after,
                                    const ItemTrade &trade) override;

  std::vector<std::int64_t> offline_admin_quarantines() override;
  bool create_offline_admin_quarantine(std::int64_t character_id,
                                        const std::string &token) override;
  bool clear_offline_admin_quarantine(std::int64_t character_id,
                                       const std::string &token) override;

  std::optional<CharacterItem> grant_item(std::int64_t account_id,
                                          std::int64_t character_id, int world_id,
                                          CharacterItem item) override;

  bool update_item(std::int64_t account_id, std::int64_t character_id, int world_id,
                   const CharacterItem &item) override;

  bool update_items(std::int64_t account_id, std::int64_t character_id, int world_id,
                    std::int64_t expected_revision,
                    const std::vector<CharacterItem> &items) override;

  bool commit_world_death(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      std::int64_t expected_revision,
      const std::vector<EquippedDurabilityChange> &durability_changes) override;

  bool commit_world_revival(std::int64_t account_id, std::int64_t character_id,
                            int world_id, std::int64_t expected_revision,
                            const WorldPosition &position) override;

  bool commit_world_shrine_revival(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      const WorldProgress &before, const WorldProgress &after,
      const WorldPosition &position) override;

  bool discard_items(std::int64_t account_id, std::int64_t character_id, int world_id,
                     std::int64_t expected_revision,
                     const std::vector<std::int64_t> &item_ids,
                     const std::map<int, int> &stackable_counts) override;

  bool apply_item_polish(std::int64_t account_id, std::int64_t character_id, int world_id,
                         std::int64_t expected_revision,
                         const ItemPolishChange &change) override;

  bool remove_item(std::int64_t account_id, std::int64_t character_id, int world_id,
                   std::int64_t item_id) override;

  WorldProgress load_world_progress(std::int64_t account_id, std::int64_t character_id,
                                    int world_id) override;

  bool save_world_progress(std::int64_t account_id, std::int64_t character_id, int world_id,
                           const WorldProgress &before, const WorldProgress &after) override;

  bool fence_world_entry(std::int64_t account_id, std::int64_t character_id,
                         int world_id) override;

  std::optional<DeathReconciliationSnapshot> fence_world_death_reconciliation(
      std::int64_t account_id, std::int64_t character_id, int world_id) override;

  bool commit_world_progress_and_position(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      const WorldProgress &before, const WorldProgress &after,
      const WorldPosition &position) override;

  std::optional<std::vector<CharacterItem>>
  commit_item_trade(std::int64_t account_id, std::int64_t character_id, int world_id,
                    const WorldProgress &before, const WorldProgress &after,
                    const ItemTrade &trade) override;

  bool enqueue_monster_rewards(const std::vector<MonsterRewardReceipt> &receipts) override;
  bool enqueue_monster_rewards_with_script(
      const std::vector<MonsterRewardReceipt> &receipts,
      const std::optional<BossScriptIntent> &intent) override;
  std::vector<BossScriptIntent> pending_boss_scripts(
      int world_id, const std::optional<std::string> &after, std::size_t limit) override;
  BossScriptSettlement settle_shared_boss_script(const BossScriptIntent &intent) override;
  bool mark_boss_script_published(const BossScriptIntent &intent) override;
  bool mark_private_boss_script_applied(const BossScriptIntent &intent) override;
  bool retire_private_boss_script(const BossScriptIntent &intent) override;
  std::int64_t acquire_monster_reward_owner_epoch() override;
  void adopt_monster_reward_owner_epoch(std::int64_t epoch) override;
  bool mark_monster_reward_ready(const std::string &receipt_id) override;
  std::size_t recover_staged_monster_rewards(std::size_t limit) override;
  bool has_staged_monster_rewards() override;
  std::vector<MonsterRewardReceipt>
  pending_monster_rewards(std::int64_t account_id, std::int64_t character_id, int world_id) override;
  MonsterRewardCommitResult commit_monster_reward(
      std::int64_t account_id, std::int64_t character_id, int world_id,
      const std::string &receipt_id,
      const WorldProgress &before, const std::optional<WorldProgress> &after,
      const ItemTrade &trade) override;

private:
  std::string connection_;
  std::atomic<std::int64_t> monster_reward_owner_epoch_{};
};

// Runs schema SQL under the migration advisory lock (7342091001) that serializes migrations
// within the same transaction as the schema changes. Shared by the server startup migration
// and the isolated-cluster tests; any repository's connection string is a valid `connection`.
void migrate_postgres(const std::string &connection, const std::string &sql);

} // namespace chronicles
