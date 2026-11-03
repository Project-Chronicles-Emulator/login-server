#pragma once

// Character-state value types shared by the character and character-state repository
// signatures: items, trades, builds, world progress and their persisted bounds.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace chronicles {

struct WorldPosition {
  int map_id{};
  int x_mm{};
  int y_mm{};
  int z_mm{};
  float yaw{};

  bool operator==(const WorldPosition &) const = default;
};

struct CharacterItem {
  std::int64_t id{};
  int cms_id{};
  int inventory_slot{};
  std::optional<int> equip_slot;
  int quantity{1};
  int durability{10000};
  int grade{};
  // Polish level (ItemPolish.jsonl target step minus one), 0 when never polished.
  int polish_step{};
  // Creation time in 100 ns Unix-epoch ticks (the unit of Item.created_epochtime); zero means
  // the instance predates creation-time tracking and the wire field is omitted.
  std::int64_t created_epoch_ticks{};

  bool operator==(const CharacterItem &) const = default;
};

// Item-row changes that a trade (NPC shop purchase, sale or repair) commits together with the
// world-progress transition. Granted items are new, unequipped instances.
struct ItemDurabilityUpdate {
  std::int64_t item_id{};
  int expected_durability{};
  int durability{};
};

struct ItemTrade {
  std::vector<CharacterItem> granted_items;
  std::vector<std::int64_t> removed_item_ids;
  std::vector<ItemDurabilityUpdate> durability_updates;
};

struct MonsterRewardQuestAttempt {
  int attempt{};
  std::int64_t started_epoch_ms{};
  bool complete{};
  bool abandoned{};
  std::map<int, int> objectives;

  bool operator==(const MonsterRewardQuestAttempt &) const = default;
};

// Durable loot receipts report the existing claimant separately from a refused write.
enum class LootClaimStatus {
  not_found,
  committed,
  already_claimed_by_same_character,
  claimed_by_other_character,
  conflict,
  source_conflict,
  mismatch,
};

inline constexpr std::size_t kMaximumRetainedLootClaimsPerWorld = 1000000;

// The sole supported linked script removes the Cliffs cannon barrier. A missing phase
// describes an unverified initial state; no guessed pre-script phase is published.
struct SharedGatheringLinkedTransition {
  int map_id{};
  int kind{};
  std::uint32_t sector_id{};
  std::uint32_t local_index{};
  std::int64_t expected_generation{};
  bool expected_active{};
  std::optional<int> expected_phase;

  bool operator==(const SharedGatheringLinkedTransition &) const = default;
};

// A shared-map source is versioned through every interaction and timed/scripted change.
// Private instance copies use fresh incarnations and do not use this persistent key.
struct SharedGatheringSourceTransition {
  int map_id{};
  int kind{};
  std::uint32_t sector_id{};
  std::uint32_t local_index{};
  std::int64_t expected_generation{};
  bool expected_active{};
  std::optional<int> expected_phase;
  bool resulting_active{};
  std::optional<int> resulting_phase;
  // The persisted deadline is armed only after the claim outcome is confirmed.
  std::optional<std::int64_t> next_transition_delay_ticks;
  std::optional<int> timed_successor_phase;
  std::optional<SharedGatheringLinkedTransition> linked;

  bool operator==(const SharedGatheringSourceTransition &) const = default;
};

inline constexpr std::size_t kMaximumSharedGatheringSourcesPerWorld = 1000000;
inline constexpr std::size_t kSharedGatheringSourcePageSize = 4096;

struct StoredSharedGatheringSource {
  std::int64_t generation{};
  bool active{};
  std::optional<int> phase;
  std::optional<std::int64_t> next_transition_epoch_ticks;
  std::optional<int> timed_successor_phase;
  std::optional<std::string> pending_receipt_id;
  std::optional<std::int64_t> next_transition_delay_ticks;
};

enum class GatheringFinalizeStatus { finalized, already_finalized, conflict };

struct GatheringFinalizeResult {
  GatheringFinalizeStatus status{GatheringFinalizeStatus::conflict};
  std::optional<std::int64_t> transition_epoch_ticks;
  // Current fenced state; it can have advanced since the receipt was finalized.
  std::optional<StoredSharedGatheringSource> source_snapshot;
  // The linked script source is fenced under the same transaction as the primary.
  std::optional<StoredSharedGatheringSource> linked_source_snapshot;
};

struct SharedGatheringSourceKey {
  int map_id{};
  int kind{};
  std::uint32_t sector_id{};
  std::uint32_t local_index{};

  auto operator<=>(const SharedGatheringSourceKey &) const = default;
};

struct SharedGatheringSourceRecord {
  SharedGatheringSourceKey key;
  StoredSharedGatheringSource state;
};

// Captured at the defeat, before publication. Only the two evidenced Cliffs bosses
// have durable script effects; their target sources are derived from these fields.
struct BossScriptIntent {
  std::string event_id;
  int world_id{};
  int map_id{};
  int npc_cms_id{};
  // "shared" or a unique process/copy incarnation. Private copies are fresh on restart.
  std::string copy_scope;

  bool operator==(const BossScriptIntent &) const = default;
};

struct BossScriptSettlement {
  bool committed{};
  std::vector<SharedGatheringSourceRecord> sources;
  std::string event_id;
};

struct SharedGatheringSourceUpdateResult {
  bool committed{};
  std::optional<StoredSharedGatheringSource> source;
  std::optional<StoredSharedGatheringSource> linked_source_snapshot;
};

struct LootClaimResult {
  LootClaimStatus status{LootClaimStatus::conflict};
  std::vector<CharacterItem> granted_items;
  // Set on shared-source conflict when the row exists. A pending receipt means the
  // source is still being finalized; callers must not restore the old local phase.
  std::optional<StoredSharedGatheringSource> source_snapshot;
};

// Durable idempotency record; an old character deletion must not erase its claim tombstone.
struct StoredLootClaim {
  std::int64_t account_id{};
  std::int64_t character_id{};
  int world_id{};
  std::string corpse_fingerprint;
  std::vector<CharacterItem> granted_items;
  // Existing corpse-loot receipts use the default kind. Gathering receipts bind an
  // additional full mutation fingerprint and remain nonterminal until finalized.
  std::string claim_kind{"loot"};
  std::string mutation_fingerprint;
  bool terminal{true};
  std::optional<std::int64_t> armed_deadline_epoch_ticks;
  bool shared_source{};
};

// Durable, per-recipient intent to apply one shared monster defeat. The receipt ID is stable
// across retries of the same monster life; the rest of the payload must also match on replay.
struct MonsterRewardReceipt {
  std::int64_t account_id{};
  std::int64_t character_id{};
  int world_id{};
  std::string receipt_id;
  std::uint64_t credited_unit_id{};
  int npc_cms_id{};
  WorldPosition position;
  bool award_experience{};
  std::map<int, MonsterRewardQuestAttempt> quest_attempts;
  bool allow_monster_offered_quests{};

  bool operator==(const MonsterRewardReceipt &) const = default;
};

enum class MonsterRewardCommitStatus { committed, already_processed, conflict };

struct MonsterRewardCommitResult {
  MonsterRewardCommitStatus status{MonsterRewardCommitStatus::conflict};
  std::vector<CharacterItem> granted_items;
};

// Server-calculated death wear; the old value guards against intervening item updates.
struct EquippedDurabilityChange {
  std::int64_t item_id{};
  int cms_id{};
  int equip_slot{};
  int expected_durability{};
  int durability{};
};

// Server-owned character build (see src/world/features/character/progression.hpp). Both maps are
// bounded and validated by storage. Character carries a copy so every stat calculation sees the
// persisted build; WorldProgress carries the revisioned copy that gameplay transitions change.
struct CharacterBuild {
  // Selected equipment slot (0 = Weapon_1, 1 = Weapon_2), retained across world entry.
  int active_weapon_slot{};

  // Allocated LevelUpPoint attribute bonuses keyed by x5.cms.EX5StatType Con (1) .. Tal (6).
  // DbCharacter stores the same six values as str_bonus .. tal_bonus.
  std::map<int, int> stat_bonuses;
  // Earned PerkNode ids (nodes that are not autoRegistration) and their level. Level 0 marks a
  // deactivated or reset node; perk rows are tombstoned like every other progress record.
  std::map<int, int> perks;

  bool operator==(const CharacterBuild &) const = default;
};

// Upper bounds for persisted build values. A perk tree has at most 609 nodes in this client
// build; the attribute bound matches the 0..1000 range CharacterStats accepts.
inline constexpr std::size_t kMaximumEarnedPerks = 1024;
inline constexpr int kMaximumStatBonus = 1000;
inline constexpr int kMaximumBuildPoints = 100000;
// Recovered NuiBlessingLevel catalog range; the partial offering balance has a local bound.
inline constexpr int kMaximumNuiBlessingLevel = 30;
inline constexpr int kMaximumDedicatedNuiTears = 100000;

struct QuestProgress {
  bool complete{};
  int success_count{};
  std::map<int, int> objectives;
  // Local monster instance IDs already credited to this attempt of the quest.
  std::set<std::uint64_t> credited_kills;
  // Restarts of this quest (abandon, daily job board repetition). Storage accepts a reset of the
  // completion flag, objective counters and kill receipts only together with a higher attempt.
  int attempt{};
  // An abandoned record is kept as a tombstone (progress rows are never deleted); the quest
  // service treats it as not accepted.
  bool abandoned{};
  // Unix epoch milliseconds at which the current attempt was accepted (TimeElapsed objectives,
  // time-attack quests); zero for records saved before it was tracked.
  std::int64_t started_epoch_ms{};

  bool operator==(const QuestProgress &) const = default;
};

// Daily state of one quest job board (QuestJobBoard.id) for a character. `day` is the board
// refresh period index (QuestJobBoardRefreshSchedule*); `refreshes` counts the paid refreshes
// bought in that period and selects a different set of offered quests.
struct JobBoardRecord {
  std::int64_t day{};
  int refreshes{};

  bool operator==(const JobBoardRecord &) const = default;
};

struct UtilitySlot {
  int item_cms_id{};
  int count{};

  bool operator==(const UtilitySlot &) const = default;
};

// Identifies one purchase-limited NPC shop row (catalog Shop.id plus Shop.FK_itemId).
struct ShopLimitKey {
  int shop_id{};
  int item_cms_id{};

  auto operator<=>(const ShopLimitKey &) const = default;
};

// Purchases counted against Shop.limitAmount in the current reset window.
struct ShopPurchaseRecord {
  int purchased_count{};
  // Exclusive end of the counting window in 100 ns Unix-epoch ticks, the unit of
  // ShopItemLimit.reset_epochtime. Zero means the limit never resets.
  std::int64_t reset_epoch_ticks{};

  bool operator==(const ShopPurchaseRecord &) const = default;
};

// Owned housing objects, reported to the client as HousingItemCount (see
// housing_item_count_json for the wire meaning of the two counts).
struct HousingItemRecord {
  // Every owned copy, placed or not.
  int quantity{};
  // Copies currently placed on a lot (never more than quantity).
  int installed_count{};

  bool operator==(const HousingItemRecord &) const = default;
};

// One trade voucher (TradeVoucher on the wire). Redeemed vouchers stay as tombstones because
// world progress records are never deleted.
struct TradeVoucherRecord {
  int good_cms_id{};
  std::int64_t created_epoch_ticks{};
  std::int64_t expiration_epoch_ticks{};
  bool redeemed{};

  bool operator==(const TradeVoucherRecord &) const = default;
};

// A registered house mate (MateInfo on the wire). Mates are never unregistered.
struct MateRecord {
  // x5.cms.EX5MateStateType: 1 BeAway, 2 Spawned, 3 Invited, 4 Follow.
  int state{1};
  int housing_lot_number{};
  int intimacy{};

  bool operator==(const MateRecord &) const = default;
};

// One trade good unit carried on a trade run (one production_epochtimes entry of TradeGoods).
struct TradeGoodUnit {
  int good_cms_id{};
  // The trading post where the unit was produced or loaded (TradeGoods.trading_post_cms_id).
  int origin_post_id{};
  // 100 ns Unix-epoch ticks, the unit of TradeGoods.production_epochtimes.
  std::int64_t production_epoch_ticks{};

  bool operator==(const TradeGoodUnit &) const = default;
};

// Stage of a character's trade run.
enum class TradeRunPhase : int {
  // No goods are loaded and no wagon is out.
  idle = 0,
  // Goods are loaded at a trading post but the wagon has not departed.
  loading = 1,
  // The wagon departed and is on the road.
  running = 2,
  // The wagon is parked inside a trading post's async sphere.
  parked = 3,
  // The wagon was stowed at the parked post; only selling remains.
  stowed = 4,
};

// The character's single trade run. The goods and the wagon transform survive reconnection;
// the wagon unit itself is respawned at the stored transform.
struct TradeRunRecord {
  TradeRunPhase phase{TradeRunPhase::idle};
  int start_post_id{};
  int parked_post_id{};
  int vehicle_cms_id{};
  std::int64_t started_epoch_ticks{};
  std::vector<TradeGoodUnit> goods;
  // Last known wagon transform while phase is running or parked.
  std::optional<WorldPosition> wagon_position;
  int wagon_health{};
  // Last wagon spawn or escape, for TradeWagonRespawnCooldown.
  std::int64_t last_wagon_spawn_epoch_ticks{};

  bool operator==(const TradeRunRecord &) const = default;
};

struct AchievementRecord {
  int state{};
  std::int64_t completion_tick{};

  bool operator==(const AchievementRecord &) const = default;
};

// Durable, bounded character state. Item instances remain in Character::items;
// consumable/material quantities use one stack per CMS definition.
struct WorldProgress {
  std::int64_t revision{};
  bool dead{};
  std::int64_t gold{};
  std::int64_t hersander_token{};
  // Cumulative character EXP (the wire `exp`), the level it reached, and the unspent
  // build points keyed by x5.cms.EX5CharBuildPointType (LevelUpPoint, WeaponPoint).
  std::int64_t experience{};
  int level{1};
  int level_up_points{};
  int weapon_points{};
  int nui_blessing_level{};
  int dedicated_nui_tears{};
  CharacterBuild build;
  std::map<int, int> stackables;
  std::map<int, int> adventure_objects;
  std::map<int, QuestProgress> quests;
  std::map<std::uint64_t, int> doodad_phases;
  std::array<UtilitySlot, 4> utility_slots{};
  std::map<ShopLimitKey, ShopPurchaseRecord> shop_purchases;
  std::map<int, HousingItemRecord> housing_items;
  std::map<std::int64_t, TradeVoucherRecord> trade_vouchers;
  // SummonNpc ids the character registered (WorldCharSummonNpcsNoti). Never removed.
  std::set<int> registered_summons;
  // MateNpc id -> registered mate (WorldCharMatesNoti). Never removed.
  std::map<int, MateRecord> mates;
  TradeRunRecord trade_run;
  // CUSTOM (guessed, not official): NPC CMS IDs whose first-kill loot groups this character has
  // already received. Written with the corpse take and never removed. See docs/monster-loot.md.
  std::set<int> loot_first_kills;
  // Emotion ids granted beyond the Basic rows the client always owns (WorldCharEmotionsNoti,
  // EmotionsUpdateNoti). Never removed. See docs/emotes-and-selfie.md.
  std::set<int> owned_emotions;

  // ItemCraft ids learned from owned recipe items. Consuming the item and inserting this
  // receipt share one world-progress transaction; reconnect restores WorldCharItemCraftsNoti.
  std::set<int> learned_crafts;
  // Completion and claim receipts advance monotonically and share the reward transaction.
  std::map<int, AchievementRecord> achievements;
  // QuestJobBoard.id -> the character's offered-quest period on that board.
  std::map<int, JobBoardRecord> job_boards;
  // PlayRecord.id -> cumulative count of the recorded action (monster kills of a template).
  // Counters only grow; quest PlayRecord conditions compare against them.
  std::map<int, std::int64_t> play_records;

  bool operator==(const WorldProgress &) const = default;
};

// Full progress and equipped instances read under one revision fence. The caller must retain
// exclusive character admission while it interprets or retries an uncertain death operation.
struct DeathReconciliationSnapshot {
  WorldProgress progress;
  std::vector<CharacterItem> equipped_items;
};

// Upper bound of emulator_character_items.inventory_slot; one item instance per slot.
inline constexpr int kMaximumInventorySlot = 999;

// Distinct stackable CMS IDs a character may hold in WorldProgress::stackables.
inline constexpr std::size_t kMaximumStackableKinds = 1000;

// Distinct housing object kinds and trade vouchers a character may own.
inline constexpr std::size_t kMaximumHousingItemKinds = 4096;
inline constexpr std::size_t kMaximumTradeVouchers = 4096;

// Distinct NPC kinds whose first-kill loot a character may have consumed.
inline constexpr std::size_t kMaximumLootFirstKills = 4096;

// Emotions a character may own beyond the Basic rows (emulator bound; the catalog has 12 rows).
inline constexpr std::size_t kMaximumOwnedEmotions = 256;

inline constexpr std::size_t kMaximumLearnedCrafts = 4096;
inline constexpr std::size_t kMaximumAchievements = 4096;
// QuestJobBoard has 14 rows in this build; the bound rejects corrupt state.
inline constexpr std::size_t kMaximumJobBoards = 256;
// PlayRecord has 579 rows in this build.
inline constexpr std::size_t kMaximumPlayRecords = 4096;

// Registered summons and mates a character may hold; the client tables define ten and two.
inline constexpr std::size_t kMaximumRegisteredSummons = 256;
inline constexpr std::size_t kMaximumRegisteredMates = 64;

// Trade good units one run may carry. Vehicle 200 loads seven; the bound covers larger rows.
inline constexpr std::size_t kMaximumTradeRunGoods = 64;

// Throws std::invalid_argument when an item is outside the persisted value ranges.
void validate_character_item(const CharacterItem &item);

// Throws std::invalid_argument when a build has an unknown attribute, an out-of-range value or
// more perk rows than storage accepts.
void validate_character_build(const CharacterBuild &build);

// Highest polishStep in Catalog/Tables/ItemPolish.jsonl (steps 1 to 15).
inline constexpr int kMaxItemPolishStep = 15;

// Result of one tempering attempt. The numbers are x5.pb.ItemRollResult values, which the
// client's result popup switches on (AAC_ItemPolishResultPopup_SetResult).
enum class PolishOutcome {
  // The item moves to expected_polish_step + 1.
  success = 0,
  // The item keeps expected_polish_step ("Tempering level remains the same.").
  fail = 1,
  // The item instance is deleted ("Equipment destroyed."); only an unequipped item qualifies.
  destruction = 2,
};

// One tempering attempt. Every outcome charges the material stack, the optional additive and
// gold. The values come from InventoryRules::plan_polish and the server's roll.
struct ItemPolishChange {
  std::int64_t item_id{};
  int cms_id{};
  int expected_polish_step{};
  int material_cms_id{};
  int material_count{};
  std::int64_t gold_cost{};
  // One unit of this stackable is charged when nonzero.
  int additive_cms_id{};
  PolishOutcome outcome{PolishOutcome::success};
};

} // namespace chronicles
