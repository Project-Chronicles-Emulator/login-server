#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"
#include "persistence/loot_claim_internal.hpp"
#include "persistence/quest_service_effect_codec.hpp"
#include "persistence/quest_service_effect_guards.hpp"
#include "common/currency.hpp"
#include "data/starter_gear.hpp"
#include "persistence/postgres/character_rows.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

namespace chronicles {

std::vector<std::int64_t> MemoryCharacterStateRepository::offline_admin_quarantines() {
  std::lock_guard lock(database_->mutex);
  std::vector<std::int64_t> characters;
  characters.reserve(database_->offline_admin_quarantines.size());
  for (const auto &[character_id, token] : database_->offline_admin_quarantines) {
    characters.push_back(character_id);
  }
  return characters;
}

bool MemoryCharacterStateRepository::create_offline_admin_quarantine(
    std::int64_t character_id, const std::string &token) {
  std::lock_guard lock(database_->mutex);
  return database_->offline_admin_quarantines.emplace(character_id, token).second;
}

bool MemoryCharacterStateRepository::clear_offline_admin_quarantine(
    std::int64_t character_id, const std::string &token) {
  std::lock_guard lock(database_->mutex);
  const auto entry = database_->offline_admin_quarantines.find(character_id);
  if (entry == database_->offline_admin_quarantines.end() || entry->second != token) {
    return false;
  }
  database_->offline_admin_quarantines.erase(entry);
  return true;
}

std::vector<std::int64_t> PostgresCharacterStateRepository::offline_admin_quarantines() {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows = transaction.exec("SELECT character_id FROM emulator_offline_admin_quarantine");
  std::vector<std::int64_t> characters;
  characters.reserve(rows.size());
  for (const auto &row : rows) {
    characters.push_back(row[0].as<std::int64_t>());
  }
  return characters;
}

bool PostgresCharacterStateRepository::create_offline_admin_quarantine(
    std::int64_t character_id, const std::string &token) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  const auto rows = transaction.exec(
      "INSERT INTO emulator_offline_admin_quarantine(character_id, token) VALUES ($1, $2) "
      "ON CONFLICT (character_id) DO NOTHING RETURNING character_id",
      pqxx::params{character_id, token});
  transaction.commit();
  return !rows.empty();
}

bool PostgresCharacterStateRepository::clear_offline_admin_quarantine(
    std::int64_t character_id, const std::string &token) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  const auto rows = transaction.exec(
      "DELETE FROM emulator_offline_admin_quarantine WHERE character_id=$1 AND token=$2 "
      "RETURNING character_id", pqxx::params{character_id, token});
  transaction.commit();
  return !rows.empty();
}

namespace {

// Equal to the quest book's own bound (kMaximumSavedQuests). The client catalog has 1,571
// quests, so a character that completed every quest (the graduation grant) needs more than the
// earlier 1,024 records.
constexpr std::size_t kMaximumQuestRecords = 4096;
constexpr std::size_t kMaximumObjectRecords = 4096;
constexpr std::size_t kMaximumShopPurchaseRecords = 4096;
// Same level bound as the operator console's direct progress edit (validate_progress).
constexpr int kMaximumStoredLevel = 1000;
// One request can grant or remove only a bounded number of item rows.
constexpr std::size_t kMaximumTradedItemRows = 64;
constexpr std::size_t kMaximumMonsterRewardRecipients = 64;
constexpr std::size_t kMaximumMonsterRewardObjectiveValues = 65536;
constexpr std::size_t kMaximumMonsterRewardBatchObjectiveValues = 262144;

bool valid_monster_reward_id(const std::string &receipt_id) {
  return receipt_id.size() == 32 && std::ranges::all_of(receipt_id, [](char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
  });
}

void require_memory_reward_owner(const MemoryDatabase &database, std::int64_t owner_epoch) {
  if (owner_epoch <= 0 || database.monster_reward_owner_epoch != owner_epoch) {
    throw std::runtime_error("Monster reward owner epoch is stale or missing");
  }
}

void validate_monster_reward(const MonsterRewardReceipt &receipt) {
  const bool valid_id = valid_monster_reward_id(receipt.receipt_id);
  if (receipt.account_id <= 0 || receipt.character_id <= 0 || receipt.world_id <= 0 || !valid_id ||
      receipt.credited_unit_id == 0 ||
      receipt.credited_unit_id >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      receipt.npc_cms_id <= 0 || receipt.position.map_id <= 0 ||
      !std::isfinite(receipt.position.yaw)) {
    throw std::invalid_argument("Invalid monster reward receipt");
  }
  if (receipt.quest_attempts.size() > kMaximumQuestRecords) {
    throw std::invalid_argument("Monster reward quest snapshot exceeds storage limits");
  }
  std::size_t objective_count = 0;
  for (const auto &[quest_id, attempt] : receipt.quest_attempts) {
    if (quest_id <= 0 || attempt.attempt < 0 || attempt.started_epoch_ms < 0 ||
        (attempt.complete && attempt.abandoned) || attempt.objectives.size() > 64) {
      throw std::invalid_argument("Invalid monster reward quest attempt");
    }
    objective_count += attempt.objectives.size();
    if (objective_count > kMaximumMonsterRewardObjectiveValues) {
      throw std::invalid_argument("Monster reward objective snapshot exceeds storage limits");
    }
    for (const auto &[sequence, count] : attempt.objectives) {
      if (sequence <= 0 || count < 0) {
        throw std::invalid_argument("Invalid monster reward quest objective");
      }
    }
  }
}

nlohmann::json monster_reward_attempts_json(
    const std::map<int, MonsterRewardQuestAttempt> &attempts) {
  auto result = nlohmann::json::array();
  for (const auto &[quest_id, attempt] : attempts) {
    auto objectives = nlohmann::json::array();
    for (const auto &[sequence, count] : attempt.objectives) {
      objectives.push_back({{"sequence", sequence}, {"count", count}});
    }
    result.push_back({{"quest_id", quest_id}, {"attempt", attempt.attempt},
                      {"started_epoch_ms", attempt.started_epoch_ms},
                      {"complete", attempt.complete}, {"abandoned", attempt.abandoned},
                      {"objectives", std::move(objectives)}});
  }
  return result;
}

std::map<int, MonsterRewardQuestAttempt> monster_reward_attempts_from_json(
    const nlohmann::json &value) {
  if (!value.is_array() || value.size() > kMaximumQuestRecords) {
    throw std::runtime_error("Invalid stored monster reward quest snapshot");
  }
  std::map<int, MonsterRewardQuestAttempt> attempts;
  for (const auto &entry : value) {
    const int quest_id = entry.at("quest_id").get<int>();
    MonsterRewardQuestAttempt attempt{entry.at("attempt").get<int>(),
                                     entry.at("started_epoch_ms").get<std::int64_t>(),
                                     entry.at("complete").get<bool>(),
                                     entry.at("abandoned").get<bool>()};
    const auto &objectives = entry.at("objectives");
    if (!objectives.is_array() || objectives.size() > 64) {
      throw std::runtime_error("Invalid stored monster reward objectives");
    }
    for (const auto &objective : objectives) {
      if (!attempt.objectives.emplace(objective.at("sequence").get<int>(),
                                      objective.at("count").get<int>()).second) {
        throw std::runtime_error("Duplicate stored monster reward objective");
      }
    }
    if (!attempts.emplace(quest_id, attempt).second) {
      throw std::runtime_error("Duplicate stored monster reward quest attempt");
    }
  }
  return attempts;
}

void validate_monster_reward_batch(const std::vector<MonsterRewardReceipt> &receipts) {
  if (receipts.empty() || receipts.size() > kMaximumMonsterRewardRecipients) {
    throw std::invalid_argument("Invalid monster reward recipient count");
  }
  std::set<std::pair<std::int64_t, std::string>> keys;
  std::size_t objective_count = 0;
  for (const auto &receipt : receipts) {
    validate_monster_reward(receipt);
    if (receipt.receipt_id != receipts.front().receipt_id ||
        receipt.credited_unit_id != receipts.front().credited_unit_id ||
        receipt.npc_cms_id != receipts.front().npc_cms_id ||
        receipt.world_id != receipts.front().world_id ||
        receipt.position.map_id != receipts.front().position.map_id) {
      throw std::invalid_argument("Monster reward batch must describe one defeat");
    }
    if (!keys.emplace(receipt.character_id, receipt.receipt_id).second) {
      throw std::invalid_argument("Duplicate monster reward recipient");
    }
    for (const auto &entry : receipt.quest_attempts) {
      objective_count += entry.second.objectives.size();
      if (objective_count > kMaximumMonsterRewardBatchObjectiveValues) {
        throw std::invalid_argument("Monster reward batch objective snapshot exceeds limits");
      }
    }
  }
}

void validate_boss_script_intent(const BossScriptIntent &intent) {
  constexpr int kCliffsMap = 30104;
  constexpr int kCliffsElite = 5001510;
  constexpr int kCliffsCaptain = 5001104;
  const auto &scope = intent.copy_scope;
  const bool private_scope = scope.size() >= 34 && scope.size() <= 53 && scope[32] == ':' &&
      std::ranges::all_of(scope.substr(0, 32), [](char digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
      }) && scope[33] >= '1' && scope[33] <= '9' &&
      std::ranges::all_of(scope.substr(33), [](char digit) {
        return digit >= '0' && digit <= '9';
      });
  if (!valid_monster_reward_id(intent.event_id) || intent.world_id <= 0 ||
      intent.map_id != kCliffsMap ||
      (intent.npc_cms_id != kCliffsElite && intent.npc_cms_id != kCliffsCaptain) ||
      (scope != "shared" && !private_scope)) {
    throw std::invalid_argument("Invalid boss script intent");
  }
}

void validate_monster_reward_with_script(const std::vector<MonsterRewardReceipt> &receipts,
                                         const std::optional<BossScriptIntent> &intent) {
  if (intent) {
    validate_boss_script_intent(*intent);
  }
  if (receipts.empty()) {
    if (!intent) {
      throw std::invalid_argument("Monster reward batch has no recipients or script");
    }
    return;
  }
  validate_monster_reward_batch(receipts);
  if (intent && (intent->event_id != receipts.front().receipt_id ||
                 intent->world_id != receipts.front().world_id ||
                 intent->map_id != receipts.front().position.map_id ||
                 intent->npc_cms_id != receipts.front().npc_cms_id)) {
    throw std::invalid_argument("Boss script does not match its defeat batch");
  }
}

nlohmann::json boss_script_json(const BossScriptIntent &intent) {
  return {{"event_id", intent.event_id}, {"world_id", intent.world_id},
          {"map_id", intent.map_id}, {"npc_cms_id", intent.npc_cms_id},
          {"copy_scope", intent.copy_scope}};
}

BossScriptIntent boss_script_from_json(const nlohmann::json &value) {
  if (!value.is_object() || value.size() != 5) {
    throw std::runtime_error("Invalid stored boss script intent");
  }
  BossScriptIntent intent{value.at("event_id").get<std::string>(),
                          value.at("world_id").get<int>(), value.at("map_id").get<int>(),
                          value.at("npc_cms_id").get<int>(),
                          value.at("copy_scope").get<std::string>()};
  validate_boss_script_intent(intent);
  return intent;
}

struct BossScriptTarget {
  SharedGatheringSourceKey key;
  int phase{};
};

std::vector<BossScriptTarget> boss_script_targets(const BossScriptIntent &intent) {
  validate_boss_script_intent(intent);
  if (intent.copy_scope != "shared") {
    throw std::invalid_argument("Private copy scripts cannot mutate shared sources");
  }
  constexpr std::uint32_t kEliteSector = 170 | (289 << 14);
  constexpr std::uint32_t kRiftSector = 170 | (290 << 14);
  if (intent.npc_cms_id == 5001510) {
    return {{{intent.map_id, 1, kEliteSector, 5}, 2},
            {{intent.map_id, 1, kEliteSector, 8}, 1}};
  }
  return {{{intent.map_id, 1, kRiftSector, 2}, 1}};
}

// x5.cms.EX5MateStateType values a registered mate can hold (BeAway .. Follow).
constexpr int kFirstMateState = 1;
constexpr int kLastMateState = 4;

void validate_companions(const WorldProgress &state) {
  if (state.registered_summons.size() > kMaximumRegisteredSummons ||
      state.mates.size() > kMaximumRegisteredMates) {
    throw std::invalid_argument("Companion records exceed storage limits");
  }
  for (const auto summon_id : state.registered_summons) {
    if (summon_id <= 0) {
      throw std::invalid_argument("Invalid registered summon");
    }
  }
  for (const auto &[mate_id, mate] : state.mates) {
    if (mate_id <= 0 || mate.state < kFirstMateState || mate.state > kLastMateState ||
        mate.housing_lot_number < 0 || mate.intimacy < 0) {
      throw std::invalid_argument("Invalid mate record");
    }
  }
}

void validate_loot_first_kills(const WorldProgress &state) {
  if (state.loot_first_kills.size() > kMaximumLootFirstKills) {
    throw std::invalid_argument("Loot first-kill records exceed storage limits");
  }
  for (const auto npc_cms_id : state.loot_first_kills) {
    if (npc_cms_id <= 0) {
      throw std::invalid_argument("Invalid loot first-kill record");
    }
  }
}

void validate_owned_emotions(const WorldProgress &state) {
  if (state.owned_emotions.size() > kMaximumOwnedEmotions) {
    throw std::invalid_argument("Owned emotion records exceed storage limits");
  }
  for (const auto emotion_id : state.owned_emotions) {
    if (emotion_id <= 0) {
      throw std::invalid_argument("Invalid owned emotion record");
    }
  }
}

void validate_learned_crafts(const WorldProgress &state) {
  if (state.learned_crafts.size() > kMaximumLearnedCrafts ||
      std::ranges::any_of(state.learned_crafts, [](int craft_id) { return craft_id <= 0; })) {
    throw std::invalid_argument("Learned craft records exceed storage limits");
  }
}

void validate_trade_run(const TradeRunRecord &run) {
  const auto phase = static_cast<int>(run.phase);
  if (phase < static_cast<int>(TradeRunPhase::idle) ||
      phase > static_cast<int>(TradeRunPhase::stowed) ||
      run.goods.size() > kMaximumTradeRunGoods || run.start_post_id < 0 ||
      run.parked_post_id < 0 || run.vehicle_cms_id < 0 || run.started_epoch_ticks < 0 ||
      run.wagon_health < 0 || run.last_wagon_spawn_epoch_ticks < 0) {
    throw std::invalid_argument("Invalid trade run record");
  }
  if (run.phase == TradeRunPhase::idle && (!run.goods.empty() || run.wagon_position)) {
    throw std::invalid_argument("An idle trade run carries no goods or wagon");
  }
  for (const auto &unit : run.goods) {
    if (unit.good_cms_id <= 0 || unit.origin_post_id <= 0 || unit.production_epoch_ticks < 0) {
      throw std::invalid_argument("Invalid trade good unit");
    }
  }
}

void validate(const WorldProgress &state) {
  // Gold and HersanderToken share the catalog limit kMoneyLimit (Money.jsonl, both rows,
  // key limitUserMax = limitSystemMax). Out-of-range balances are rejected, never clamped.
  if (state.revision < 0 || state.gold < 0 || state.gold > kMoneyLimit ||
      state.hersander_token < 0 || state.hersander_token > kMoneyLimit || state.experience < 0 ||
      state.stackables.size() > kMaximumStackableKinds || state.quests.size() > kMaximumQuestRecords ||
      state.adventure_objects.size() > kMaximumObjectRecords ||
      state.doodad_phases.size() > kMaximumObjectRecords) {
    throw std::invalid_argument("World progress exceeds storage limits");
  }
  if (state.level < 1 || state.level > kMaximumStoredLevel || state.level_up_points < 0 ||
      state.level_up_points > kMaximumBuildPoints || state.weapon_points < 0 ||
      state.weapon_points > kMaximumBuildPoints) {
    throw std::invalid_argument("Character level or build points exceed storage limits");
  }
  if (state.nui_blessing_level < 0 ||
      state.nui_blessing_level > kMaximumNuiBlessingLevel || state.dedicated_nui_tears < 0 ||
      state.dedicated_nui_tears > kMaximumDedicatedNuiTears) {
    throw std::invalid_argument("Nui blessing progression exceeds storage limits");
  }
  validate_character_build(state.build);
  for (const auto &[cms_id, quantity] : state.stackables) {
    if (cms_id <= 0 || quantity < 0) {
      throw std::invalid_argument("Invalid stackable progress");
    }
  }
  for (const auto &[cms_id, object_state] : state.adventure_objects) {
    if (cms_id <= 0 || object_state < 1 || object_state > 2) {
      throw std::invalid_argument("Invalid adventure object progress");
    }
  }
  for (const auto &[cms_id, quest] : state.quests) {
    if (cms_id <= 0 || quest.success_count < 0 || quest.objectives.size() > 64 ||
        quest.credited_kills.size() > 4096 || quest.attempt < 0 || quest.started_epoch_ms < 0 ||
        (quest.abandoned && quest.complete)) {
      throw std::invalid_argument("Invalid quest progress");
    }
    for (const auto unit_id : quest.credited_kills) {
      if (unit_id == 0 || unit_id > std::numeric_limits<std::int64_t>::max()) {
        throw std::invalid_argument("Invalid quest kill receipt");
      }
    }
    for (const auto &[sequence, value] : quest.objectives) {
      if (sequence <= 0 || value < 0) {
        throw std::invalid_argument("Invalid quest objective progress");
      }
    }
  }
  for (const auto &[unit_id, phase] : state.doodad_phases) {
    if (unit_id == 0 || unit_id > std::numeric_limits<std::int64_t>::max() || phase < 0) {
      throw std::invalid_argument("Invalid doodad progress");
    }
  }
  for (const auto &slot : state.utility_slots) {
    if (slot.item_cms_id < 0 || slot.count < 0) {
      throw std::invalid_argument("Invalid utility bag progress");
    }
  }
  if (state.shop_purchases.size() > kMaximumShopPurchaseRecords) {
    throw std::invalid_argument("Shop purchase records exceed storage limits");
  }
  for (const auto &[key, record] : state.shop_purchases) {
    if (key.shop_id <= 0 || key.item_cms_id <= 0 || record.purchased_count < 0 ||
        record.reset_epoch_ticks < 0) {
      throw std::invalid_argument("Invalid shop purchase record");
    }
  }
  if (state.housing_items.size() > kMaximumHousingItemKinds ||
      state.trade_vouchers.size() > kMaximumTradeVouchers) {
    throw std::invalid_argument("Housing or voucher records exceed storage limits");
  }
  for (const auto &[item_cms_id, record] : state.housing_items) {
    if (item_cms_id <= 0 || record.quantity < 0 || record.installed_count < 0 ||
        record.installed_count > record.quantity) {
      throw std::invalid_argument("Invalid housing item record");
    }
  }
  for (const auto &[voucher_id, record] : state.trade_vouchers) {
    if (voucher_id <= 0 || record.good_cms_id <= 0 || record.created_epoch_ticks < 0 ||
        record.expiration_epoch_ticks < record.created_epoch_ticks) {
      throw std::invalid_argument("Invalid trade voucher record");
    }
  }
  validate_companions(state);
  validate_loot_first_kills(state);
  validate_owned_emotions(state);
  validate_learned_crafts(state);
  if (state.achievements.size() > kMaximumAchievements) {
    throw std::invalid_argument("Too many achievement receipts");
  }
  for (const auto &[id, record] : state.achievements) {
    if (id <= 0 || (record.state != 1 && record.state != 2) || record.completion_tick <= 0) {
      throw std::invalid_argument("Invalid achievement receipt");
    }
  }
  if (state.job_boards.size() > kMaximumJobBoards) {
    throw std::invalid_argument("Too many job board records");
  }
  for (const auto &[board_id, record] : state.job_boards) {
    if (board_id <= 0 || record.day < 0 || record.refreshes < 0) {
      throw std::invalid_argument("Invalid job board record");
    }
  }
  if (state.play_records.size() > kMaximumPlayRecords) {
    throw std::invalid_argument("Too many play records");
  }
  for (const auto &[record_id, count] : state.play_records) {
    if (record_id <= 0 || count < 0) {
      throw std::invalid_argument("Invalid play record");
    }
  }
  validate_trade_run(state.trade_run);
}

template <typename Map, typename Function>
void changed_rows(const Map &before, const Map &after, Function function) {
  for (const auto &[key, value] : after) {
    const auto previous = before.find(key);
    if (previous == before.end() || previous->second != value) {
      function(key, value);
    }
  }
  // Records are tombstoned with zero quantities/completed states. Deleting a
  // consumed-object marker would permit a reward to be granted a second time.
  for (const auto &[key, value] : before) {
    if (!after.contains(key)) {
      throw std::invalid_argument("World progress records cannot be removed");
    }
  }
}

nlohmann::json objectives_json(const std::map<int, int> &objectives) {
  auto document = nlohmann::json::object();
  for (const auto &[sequence, value] : objectives) {
    document[std::to_string(sequence)] = value;
  }
  return document;
}

void validate_transition(const WorldProgress &before, const WorldProgress &after) {
  validate(after);
  if (before.revision == std::numeric_limits<std::int64_t>::max() ||
      after.revision != before.revision + 1 || before.dead != after.dead) {
    throw std::invalid_argument("World progress must preserve life state and advance once");
  }
  const auto preserve_keys = [](const auto &key, const auto &value) {};
  changed_rows(before.stackables, after.stackables, preserve_keys);
  changed_rows(before.adventure_objects, after.adventure_objects, preserve_keys);
  changed_rows(before.quests, after.quests, preserve_keys);
  changed_rows(before.doodad_phases, after.doodad_phases, preserve_keys);
  changed_rows(before.shop_purchases, after.shop_purchases, preserve_keys);
  changed_rows(before.housing_items, after.housing_items, preserve_keys);
  changed_rows(before.trade_vouchers, after.trade_vouchers, preserve_keys);
  changed_rows(before.mates, after.mates, preserve_keys);
  changed_rows(before.achievements, after.achievements, preserve_keys);
  for (const auto &[id, record] : before.achievements) {
    const auto &next = after.achievements.at(id);
    if (next.state < record.state || next.completion_tick != record.completion_tick) {
      throw std::invalid_argument("Achievement receipts cannot be reset or retimed");
    }
  }
  changed_rows(before.build.perks, after.build.perks, preserve_keys);
  if (!std::includes(after.registered_summons.begin(), after.registered_summons.end(),
                     before.registered_summons.begin(), before.registered_summons.end())) {
    throw std::invalid_argument("Registered summons cannot be removed");
  }
  if (!std::includes(after.loot_first_kills.begin(), after.loot_first_kills.end(),
                     before.loot_first_kills.begin(), before.loot_first_kills.end())) {
    throw std::invalid_argument("Loot first-kill records cannot be removed");
  }
  if (!std::includes(after.owned_emotions.begin(), after.owned_emotions.end(),
                     before.owned_emotions.begin(), before.owned_emotions.end())) {
    throw std::invalid_argument("Owned emotions cannot be removed");
  }
  if (!std::includes(after.learned_crafts.begin(), after.learned_crafts.end(),
                     before.learned_crafts.begin(), before.learned_crafts.end())) {
    throw std::invalid_argument("Learned crafts cannot be removed");
  }
  for (const auto &[voucher_id, voucher] : before.trade_vouchers) {
    if (voucher.redeemed && !after.trade_vouchers.at(voucher_id).redeemed) {
      throw std::invalid_argument("Redeemed trade vouchers cannot become valid again");
    }
  }
  for (const auto &[cms_id, state] : before.adventure_objects) {
    if (after.adventure_objects.at(cms_id) < state) {
      throw std::invalid_argument("Consumed adventure objects cannot become available again");
    }
  }
  for (const auto &[cms_id, quest] : before.quests) {
    const auto &next = after.quests.at(cms_id);
    if (next.attempt < quest.attempt || next.success_count < quest.success_count) {
      throw std::invalid_argument("Quest attempts and success counts cannot decrease");
    }
    if (next.attempt > quest.attempt) {
      // A restarted quest (abandon or a repeated job) begins again from empty progress.
      continue;
    }
    if ((quest.complete && !next.complete) || (quest.abandoned && !next.abandoned) ||
        next.started_epoch_ms != quest.started_epoch_ms) {
      throw std::invalid_argument("Completed tutorial quests cannot be reset");
    }
    for (const auto &[sequence, value] : quest.objectives) {
      const auto objective = next.objectives.find(sequence);
      if (objective == next.objectives.end() || objective->second < value) {
        throw std::invalid_argument("Tutorial quest objectives cannot lose progress");
      }
    }
    if (!std::includes(next.credited_kills.begin(), next.credited_kills.end(),
                       quest.credited_kills.begin(), quest.credited_kills.end())) {
      throw std::invalid_argument("Tutorial quest kill receipts cannot be removed");
    }
  }
  changed_rows(before.job_boards, after.job_boards, preserve_keys);
  for (const auto &[board_id, record] : before.job_boards) {
    const auto &next = after.job_boards.at(board_id);
    if (next.day < record.day || (next.day == record.day && next.refreshes < record.refreshes)) {
      throw std::invalid_argument("Job board periods cannot move backwards");
    }
  }
  changed_rows(before.play_records, after.play_records, preserve_keys);
  for (const auto &[record_id, count] : before.play_records) {
    if (after.play_records.at(record_id) < count) {
      throw std::invalid_argument("Play records cannot decrease");
    }
  }
}

// Grants are new unequipped instances; removals name distinct existing storage IDs; durability
// updates set a non-negative value on rows that are not also removed. Repairs restore wear and
// tool interactions (gathering, butchering) wear the equipped tool, as captured officially.
void validate_item_trade(const ItemTrade &trade) {
  if (trade.granted_items.size() > kMaximumTradedItemRows ||
      trade.removed_item_ids.size() > kMaximumTradedItemRows ||
      trade.durability_updates.size() > kMaximumTradedItemRows) {
    throw std::invalid_argument("Item trade exceeds the per-request row limit");
  }

  std::set<int> granted_slots;
  for (const auto &item : trade.granted_items) {
    validate_character_item(item);
    if (item.id != 0 || item.equip_slot || !granted_slots.insert(item.inventory_slot).second) {
      throw std::invalid_argument("Granted trade items must be new, unequipped and distinct");
    }
  }

  std::set<std::int64_t> touched_item_ids;
  for (const auto item_id : trade.removed_item_ids) {
    if (item_id <= 0 || !touched_item_ids.insert(item_id).second) {
      throw std::invalid_argument("Removed trade item IDs must be positive and distinct");
    }
  }
  for (const auto &update : trade.durability_updates) {
    if (update.item_id <= 0 || !touched_item_ids.insert(update.item_id).second ||
        update.expected_durability < 0 || update.durability < 0) {
      throw std::invalid_argument("Durability updates must name distinct items, never negative");
    }
  }
}

// Trade run goods are stored as a JSON array of {good, origin, produced} objects; the row holds
// at most kMaximumTradeRunGoods entries (validated before every write).
nlohmann::json trade_goods_json(const std::vector<TradeGoodUnit> &goods) {
  auto document = nlohmann::json::array();
  for (const auto &unit : goods) {
    document.push_back({{"good", unit.good_cms_id},
                        {"origin", unit.origin_post_id},
                        {"produced", unit.production_epoch_ticks}});
  }
  return document;
}

std::vector<TradeGoodUnit> trade_goods_from_json(const nlohmann::json &document) {
  if (!document.is_array() || document.size() > kMaximumTradeRunGoods) {
    throw std::runtime_error("Invalid stored trade run goods");
  }

  std::vector<TradeGoodUnit> goods;
  for (const auto &entry : document) {
    goods.push_back({entry.at("good").get<int>(), entry.at("origin").get<int>(),
                     entry.at("produced").get<std::int64_t>()});
  }
  return goods;
}

void write_trade_run(pqxx::work &transaction, std::int64_t character_id,
                     const TradeRunRecord &run) {
  const auto &wagon = run.wagon_position;
  transaction.exec(
      "INSERT INTO emulator_character_trade_runs(character_id,phase,start_post_id,"
      "parked_post_id,vehicle_cms_id,started_epoch_ticks,goods,wagon_out,wagon_map_id,wagon_x,"
      "wagon_y,wagon_z,wagon_yaw,wagon_health,last_wagon_spawn_epoch_ticks) VALUES "
      "($1,$2,$3,$4,$5,$6,$7::jsonb,$8,$9,$10,$11,$12,$13,$14,$15) "
      "ON CONFLICT (character_id) DO UPDATE SET phase=EXCLUDED.phase,"
      "start_post_id=EXCLUDED.start_post_id,parked_post_id=EXCLUDED.parked_post_id,"
      "vehicle_cms_id=EXCLUDED.vehicle_cms_id,started_epoch_ticks=EXCLUDED.started_epoch_ticks,"
      "goods=EXCLUDED.goods,wagon_out=EXCLUDED.wagon_out,wagon_map_id=EXCLUDED.wagon_map_id,"
      "wagon_x=EXCLUDED.wagon_x,wagon_y=EXCLUDED.wagon_y,wagon_z=EXCLUDED.wagon_z,"
      "wagon_yaw=EXCLUDED.wagon_yaw,wagon_health=EXCLUDED.wagon_health,"
      "last_wagon_spawn_epoch_ticks=EXCLUDED.last_wagon_spawn_epoch_ticks",
      pqxx::params{character_id, static_cast<int>(run.phase), run.start_post_id,
                   run.parked_post_id, run.vehicle_cms_id, run.started_epoch_ticks,
                   trade_goods_json(run.goods).dump(), wagon.has_value(),
                   wagon ? wagon->map_id : 0, wagon ? wagon->x_mm : 0, wagon ? wagon->y_mm : 0,
                   wagon ? wagon->z_mm : 0, wagon ? wagon->yaw : 0.0F, run.wagon_health,
                   run.last_wagon_spawn_epoch_ticks});
}

// The six attribute bonus columns, in x5.cms.EX5StatType order Con (1) .. Tal (6).
constexpr std::array<const char *, 6> kStatBonusColumns{
    "con_bonus", "dex_bonus", "end_bonus", "int_bonus", "str_bonus", "tal_bonus"};
constexpr int kFirstBonusStatType = 1;

int stat_bonus(const CharacterBuild &build, int stat_type) {
  const auto found = build.stat_bonuses.find(stat_type);
  if (found == build.stat_bonuses.end()) {
    return 0;
  }
  return found->second;
}

// Applies a validated transition inside the caller's transaction. Returns false when the
// stored revision or ownership does not match; the caller then abandons the transaction.
bool write_world_progress(pqxx::work &transaction, std::int64_t account_id,
                          std::int64_t character_id, int world_id, const WorldProgress &before,
                          const WorldProgress &after) {
  const auto &build = after.build;
  const auto character = transaction.exec(
      "UPDATE emulator_characters SET gameplay_revision=$5,gold=$6,hersander_token=$7,"
      "experience=$8,level=$10,level_up_points=$11,weapon_points=$12,con_bonus=$13,"
      "dex_bonus=$14,end_bonus=$15,int_bonus=$16,str_bonus=$17,tal_bonus=$18,"
      "nui_blessing_level=$19,dedicated_nui_tears=$20,active_weapon_slot=$21 "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 AND gameplay_revision=$4 "
      "AND player_dead=$9 RETURNING id",
      pqxx::params{account_id,
                   character_id,
                   world_id,
                   before.revision,
                   after.revision,
                   after.gold,
                   after.hersander_token,
                   after.experience,
                   before.dead,
                   after.level,
                   after.level_up_points,
                   after.weapon_points,
                   stat_bonus(build, 1),
                   stat_bonus(build, 2),
                   stat_bonus(build, 3),
                   stat_bonus(build, 4),
                   stat_bonus(build, 5),
                   stat_bonus(build, 6),
                   after.nui_blessing_level,
                   after.dedicated_nui_tears,
                   build.active_weapon_slot});
  if (character.empty()) {
    return false;
  }
  changed_rows(before.build.perks, after.build.perks, [&](int perk_cms_id, int perk_level) {
    transaction.exec(
        "INSERT INTO emulator_character_perks(character_id,perk_cms_id,perk_level) "
        "VALUES ($1,$2,$3) "
        "ON CONFLICT (character_id,perk_cms_id) DO UPDATE SET perk_level=EXCLUDED.perk_level",
        pqxx::params{character_id, perk_cms_id, perk_level});
  });
  changed_rows(before.stackables, after.stackables, [&](int cms_id, int quantity) {
    transaction.exec(
        "INSERT INTO emulator_character_stackables(character_id,item_cms_id,quantity) VALUES "
        "($1,$2,$3) "
        "ON CONFLICT (character_id,item_cms_id) DO UPDATE SET quantity=EXCLUDED.quantity",
        pqxx::params{character_id, cms_id, quantity});
  });
  changed_rows(before.adventure_objects, after.adventure_objects, [&](int cms_id, int state) {
    transaction.exec(
        "INSERT INTO emulator_character_adventure_objects(character_id,object_cms_id,state) VALUES "
        "($1,$2,$3) "
        "ON CONFLICT (character_id,object_cms_id) DO UPDATE SET state=EXCLUDED.state",
        pqxx::params{character_id, cms_id, state});
  });
  changed_rows(before.quests, after.quests, [&](int cms_id, const QuestProgress &quest) {
    transaction.exec(
        "INSERT INTO "
        "emulator_character_quests(character_id,quest_cms_id,complete,success_count,objectives,"
        "credited_kills,attempt,abandoned,started_epoch_ms) "
        "VALUES ($1,$2,$3,$4,$5::jsonb,$6::jsonb,$7,$8,$9) "
        "ON CONFLICT (character_id,quest_cms_id) DO UPDATE SET complete=EXCLUDED.complete,"
        "success_count=EXCLUDED.success_count,objectives=EXCLUDED.objectives,"
        "credited_kills=EXCLUDED.credited_kills,attempt=EXCLUDED.attempt,"
        "abandoned=EXCLUDED.abandoned,started_epoch_ms=EXCLUDED.started_epoch_ms",
        pqxx::params{character_id, cms_id, quest.complete, quest.success_count,
                     objectives_json(quest.objectives).dump(),
                     nlohmann::json(quest.credited_kills).dump(), quest.attempt,
                     quest.abandoned, quest.started_epoch_ms});
  });
  changed_rows(before.doodad_phases, after.doodad_phases, [&](std::uint64_t unit_id, int phase) {
    transaction.exec("INSERT INTO emulator_character_doodad_phases(character_id,unit_id,phase) "
                     "VALUES ($1,$2,$3) "
                     "ON CONFLICT (character_id,unit_id) DO UPDATE SET phase=EXCLUDED.phase",
                     pqxx::params{character_id, unit_id, phase});
  });
  for (std::size_t slot = 0; slot < after.utility_slots.size(); ++slot) {
    if (after.utility_slots[slot] == before.utility_slots[slot]) {
      continue;
    }
    const auto &item = after.utility_slots[slot];
    transaction.exec(
        "INSERT INTO emulator_character_utility_slots(character_id,slot,item_cms_id,quantity) "
        "VALUES ($1,$2,$3,$4) "
        "ON CONFLICT (character_id,slot) DO UPDATE SET item_cms_id=EXCLUDED.item_cms_id,"
        "quantity=EXCLUDED.quantity",
        pqxx::params{character_id, static_cast<int>(slot), item.item_cms_id, item.count});
  }
  changed_rows(before.shop_purchases, after.shop_purchases,
               [&](const ShopLimitKey &key, const ShopPurchaseRecord &record) {
                 transaction.exec(
                     "INSERT INTO emulator_character_shop_purchases(character_id,shop_id,"
                     "item_cms_id,purchased_count,reset_epoch_ticks) VALUES ($1,$2,$3,$4,$5) "
                     "ON CONFLICT (character_id,shop_id,item_cms_id) DO UPDATE SET "
                     "purchased_count=EXCLUDED.purchased_count,"
                     "reset_epoch_ticks=EXCLUDED.reset_epoch_ticks",
                     pqxx::params{character_id, key.shop_id, key.item_cms_id,
                                  record.purchased_count, record.reset_epoch_ticks});
               });
  changed_rows(before.housing_items, after.housing_items,
               [&](int item_cms_id, const HousingItemRecord &record) {
                 transaction.exec(
                     "INSERT INTO emulator_character_housing_items(character_id,item_cms_id,"
                     "quantity,installed_count) VALUES ($1,$2,$3,$4) "
                     "ON CONFLICT (character_id,item_cms_id) DO UPDATE SET "
                     "quantity=EXCLUDED.quantity,installed_count=EXCLUDED.installed_count",
                     pqxx::params{character_id, item_cms_id, record.quantity,
                                  record.installed_count});
               });
  changed_rows(before.trade_vouchers, after.trade_vouchers,
               [&](std::int64_t voucher_id, const TradeVoucherRecord &record) {
                 transaction.exec(
                     "INSERT INTO emulator_character_trade_vouchers(character_id,voucher_id,"
                     "good_cms_id,created_epoch_ticks,expiration_epoch_ticks,redeemed) "
                     "VALUES ($1,$2,$3,$4,$5,$6) "
                     "ON CONFLICT (character_id,voucher_id) DO UPDATE SET "
                     "redeemed=EXCLUDED.redeemed",
                     pqxx::params{character_id, voucher_id, record.good_cms_id,
                                  record.created_epoch_ticks, record.expiration_epoch_ticks,
                                  record.redeemed});
               });
  for (const auto summon_id : after.registered_summons) {
    if (before.registered_summons.contains(summon_id)) {
      continue;
    }
    transaction.exec("INSERT INTO emulator_character_summons(character_id,summon_cms_id) "
                     "VALUES ($1,$2) ON CONFLICT DO NOTHING",
                     pqxx::params{character_id, summon_id});
  }
  for (const auto npc_cms_id : after.loot_first_kills) {
    if (before.loot_first_kills.contains(npc_cms_id)) {
      continue;
    }
    transaction.exec("INSERT INTO emulator_character_loot_first_kills(character_id,npc_cms_id) "
                     "VALUES ($1,$2) ON CONFLICT DO NOTHING",
                     pqxx::params{character_id, npc_cms_id});
  }
  for (const auto emotion_id : after.owned_emotions) {
    if (before.owned_emotions.contains(emotion_id)) {
      continue;
    }
    transaction.exec("INSERT INTO emulator_character_emotions(character_id,emotion_cms_id) "
                     "VALUES ($1,$2) ON CONFLICT DO NOTHING",
                     pqxx::params{character_id, emotion_id});
  }
  for (const auto craft_id : after.learned_crafts) {
    if (!before.learned_crafts.contains(craft_id)) {
      transaction.exec("INSERT INTO emulator_character_crafts(character_id,craft_cms_id) "
                       "VALUES ($1,$2) ON CONFLICT DO NOTHING",
                       pqxx::params{character_id, craft_id});
    }
  }
  changed_rows(before.mates, after.mates, [&](int mate_id, const MateRecord &mate) {
    transaction.exec(
        "INSERT INTO emulator_character_mates(character_id,mate_cms_id,state,"
        "housing_lot_number,intimacy) VALUES ($1,$2,$3,$4,$5) "
        "ON CONFLICT (character_id,mate_cms_id) DO UPDATE SET state=EXCLUDED.state,"
        "housing_lot_number=EXCLUDED.housing_lot_number,intimacy=EXCLUDED.intimacy",
        pqxx::params{character_id, mate_id, mate.state, mate.housing_lot_number,
                     mate.intimacy});
  });
  changed_rows(
      before.achievements, after.achievements, [&](int id, const AchievementRecord &record) {
        transaction.exec(
            "INSERT INTO emulator_character_achievements(character_id,achievement_cms_id,state,"
            "completion_tick) VALUES ($1,$2,$3,$4) ON CONFLICT (character_id,achievement_cms_id) "
            "DO UPDATE SET state=EXCLUDED.state",
            pqxx::params{character_id, id, record.state, record.completion_tick});
      });
  changed_rows(before.job_boards, after.job_boards, [&](int board_id, const JobBoardRecord &record) {
    transaction.exec(
        "INSERT INTO emulator_character_job_boards(character_id,board_cms_id,period,refreshes) "
        "VALUES ($1,$2,$3,$4) ON CONFLICT (character_id,board_cms_id) "
        "DO UPDATE SET period=EXCLUDED.period,refreshes=EXCLUDED.refreshes",
        pqxx::params{character_id, board_id, record.day, record.refreshes});
  });
  changed_rows(before.play_records, after.play_records, [&](int record_id, std::int64_t count) {
    transaction.exec(
        "INSERT INTO emulator_character_play_records(character_id,play_record_cms_id,count) "
        "VALUES ($1,$2,$3) ON CONFLICT (character_id,play_record_cms_id) "
        "DO UPDATE SET count=EXCLUDED.count",
        pqxx::params{character_id, record_id, count});
  });
  if (after.trade_run != before.trade_run) {
    write_trade_run(transaction, character_id, after.trade_run);
  }
  return true;
}

// Caller holds the memory database mutex and has checked the owner's revision.
std::optional<std::vector<CharacterItem>> apply_memory_trade(
    MemoryDatabase &database, Character &owner, const WorldProgress &after, const ItemTrade &trade) {
  auto items = owner.items;
  for (const auto item_id : trade.removed_item_ids) {
    const auto removed = std::ranges::find_if(items, [item_id](const CharacterItem &item) {
      return item.id == item_id;
    });
    if (removed == items.end() || removed->equip_slot) {
      return std::nullopt;
    }
    items.erase(removed);
  }

  for (const auto &update : trade.durability_updates) {
    const auto repaired = std::ranges::find_if(items, [&update](const CharacterItem &item) {
      return item.id == update.item_id;
    });
    if (repaired == items.end() || repaired->durability != update.expected_durability) {
      return std::nullopt;
    }
    repaired->durability = update.durability;
  }

  auto next_item_id = database.next_item_id;
  std::vector<CharacterItem> committed;
  committed.reserve(trade.granted_items.size());
  for (auto item : trade.granted_items) {
    const bool slot_taken = std::ranges::any_of(items, [&](const CharacterItem &existing) {
      return existing.inventory_slot == item.inventory_slot;
    });
    if (slot_taken) {
      return std::nullopt;
    }
    item.id = next_item_id++;
    items.push_back(item);
    committed.push_back(item);
  }

  detail::commit_progress_and_items(database, owner, after, std::move(items), next_item_id);
  owner.experience = after.experience;
  owner.level = after.level;
  owner.build = after.build;
  owner.nui_blessing_level = after.nui_blessing_level;
  owner.dedicated_nui_tears = after.dedicated_nui_tears;
  return committed;
}

// The caller owns the transaction. No commit occurs here, so a later receipt update can still
// roll back all item and progress changes together.
std::optional<std::vector<CharacterItem>> apply_postgres_trade(
    pqxx::work &transaction, std::int64_t account_id, std::int64_t character_id, int world_id,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade) {
  if (!write_world_progress(transaction, account_id, character_id, world_id, before, after)) {
    return std::nullopt;
  }

  for (const auto item_id : trade.removed_item_ids) {
    const auto deleted = transaction.exec(
        "DELETE FROM emulator_character_items WHERE id=$1 AND character_id=$2 "
        "AND equip_slot IS NULL RETURNING id",
        pqxx::params{item_id, character_id});
    if (deleted.empty()) {
      return std::nullopt;
    }
  }

  for (const auto &update : trade.durability_updates) {
    const auto repaired = transaction.exec(
        "UPDATE emulator_character_items SET durability=$3 "
        "WHERE id=$1 AND character_id=$2 AND durability=$4 RETURNING id",
        pqxx::params{update.item_id, character_id, update.durability,
                     update.expected_durability});
    if (repaired.empty()) {
      return std::nullopt;
    }
  }

  std::vector<CharacterItem> committed;
  committed.reserve(trade.granted_items.size());
  for (auto item : trade.granted_items) {
    const auto inserted = transaction.exec(
        "INSERT INTO emulator_character_items(character_id,item_cms_id,inventory_slot,"
        "equip_slot,quantity,durability,grade,created_epoch_ticks,polish_step) "
        "VALUES ($1,$2,$3,NULL,$4,$5,$6,$7,$8) RETURNING id",
        pqxx::params{character_id, item.cms_id, item.inventory_slot, item.quantity,
                     item.durability, item.grade, item.created_epoch_ticks, item.polish_step});
    item.id = inserted.front()[0].as<std::int64_t>();
    committed.push_back(item);
  }
  return committed;
}

} // namespace

namespace detail {

void validate_world_progress_transition(const WorldProgress &before, const WorldProgress &after) {
  validate_transition(before, after);
}

bool write_world_progress_transition(pqxx::work &transaction, std::int64_t account_id,
                                     std::int64_t character_id, int world_id,
                                     const WorldProgress &before, const WorldProgress &after) {
  return write_world_progress(transaction, account_id, character_id, world_id, before, after);
}

CharacterBuild read_character_build(pqxx::transaction_base &transaction,
                                    std::int64_t character_id) {
  CharacterBuild build;
  const auto bonuses = transaction.exec(
      "SELECT con_bonus,dex_bonus,end_bonus,int_bonus,str_bonus,tal_bonus,active_weapon_slot "
      "FROM emulator_characters WHERE id=$1",
      pqxx::params{character_id});
  if (!bonuses.empty()) {
    const auto &row = bonuses.front();
    build.active_weapon_slot = row[6].as<int>();
    for (std::size_t column = 0; column < kStatBonusColumns.size(); ++column) {
      const auto value = row[static_cast<int>(column)].as<int>();
      if (value != 0) {
        build.stat_bonuses.emplace(kFirstBonusStatType + static_cast<int>(column), value);
      }
    }
  }

  for (const auto &row : transaction.exec(
           "SELECT perk_cms_id,perk_level FROM emulator_character_perks WHERE character_id=$1",
           pqxx::params{character_id})) {
    build.perks.emplace(row[0].as<int>(), row[1].as<int>());
  }

  validate_character_build(build);
  return build;
}

} // namespace detail

void validate_character_build(const CharacterBuild &build) {
  if (build.active_weapon_slot < 0 || build.active_weapon_slot > 1) {
    throw std::invalid_argument("Invalid active weapon slot");
  }
  if (build.perks.size() > kMaximumEarnedPerks) {
    throw std::invalid_argument("Earned perks exceed storage limits");
  }
  for (const auto &[stat_type, value] : build.stat_bonuses) {
    const bool core_attribute = stat_type >= kFirstBonusStatType &&
                                stat_type < kFirstBonusStatType +
                                                static_cast<int>(kStatBonusColumns.size());
    if (!core_attribute || value < 0 || value > kMaximumStatBonus) {
      throw std::invalid_argument("Invalid allocated attribute bonus");
    }
  }
  for (const auto &[perk_cms_id, perk_level] : build.perks) {
    if (perk_cms_id <= 0 || perk_level < 0) {
      throw std::invalid_argument("Invalid earned perk record");
    }
  }
}

WorldProgress MemoryCharacterStateRepository::load_world_progress(std::int64_t account_id,
                                                                    std::int64_t character_id,
                                                                    int          world_id) {
  std::lock_guard lock(database_->mutex);
  for (const auto &character : database_->characters) {
    if (character.id == character_id && character.account_id == account_id &&
        character.world_id == world_id) {
      auto state = database_->world_progress[character_id];
      state.experience = character.experience;
      state.level = character.level;
      state.build = character.build;
      state.nui_blessing_level = character.nui_blessing_level;
      state.dedicated_nui_tears = character.dedicated_nui_tears;
      return state;
    }
  }
  throw std::runtime_error("World progress character is not owned by this account");
}

std::optional<DeathReconciliationSnapshot>
MemoryCharacterStateRepository::fence_world_death_reconciliation(
    std::int64_t account_id, std::int64_t character_id, int world_id) {
  std::lock_guard lock(database_->mutex);
  const auto owner = detail::find_owned_character(*database_, account_id, character_id, world_id);
  if (owner == database_->characters.end()) {
    return std::nullopt;
  }

  auto &stored = database_->world_progress[character_id];
  if (stored.revision == std::numeric_limits<std::int64_t>::max()) {
    return std::nullopt;
  }

  DeathReconciliationSnapshot snapshot;
  snapshot.progress = stored;
  snapshot.progress.experience = owner->experience;
  snapshot.progress.level = owner->level;
  snapshot.progress.build = owner->build;
  snapshot.progress.nui_blessing_level = owner->nui_blessing_level;
  snapshot.progress.dedicated_nui_tears = owner->dedicated_nui_tears;
  validate(snapshot.progress);
  std::set<int> equipped_slots;
  for (const auto &item : owner->items) {
    if (!item.equip_slot) {
      continue;
    }
    validate_character_item(item);
    if (item.id <= 0 || snapshot.equipped_items.size() >= static_cast<std::size_t>(kEquipSlotCount) ||
        !equipped_slots.insert(*item.equip_slot).second) {
      throw std::runtime_error("Invalid equipped items during death reconciliation");
    }
    snapshot.equipped_items.push_back(item);
  }
  std::ranges::sort(snapshot.equipped_items, {}, &CharacterItem::inventory_slot);
  snapshot.progress.revision = stored.revision + 1;
  stored.revision = snapshot.progress.revision;
  return snapshot;
}

bool MemoryCharacterStateRepository::save_world_progress(std::int64_t account_id,
                                                           std::int64_t character_id,
                                                           int                world_id,
                                                           const WorldProgress &before,
                                                           const WorldProgress &after) {
  validate_transition(before, after);
  std::lock_guard lock(database_->mutex);
  for (auto &character : database_->characters) {
    if (character.id == character_id && character.account_id == account_id &&
        character.world_id == world_id) {
      auto &stored = database_->world_progress[character_id];
      if (stored.revision != before.revision || stored.dead != before.dead) {
        return false;
      }
      stored = after;
      character.experience = after.experience;
      character.level = after.level;
      character.build = after.build;
      character.nui_blessing_level = after.nui_blessing_level;
      character.dedicated_nui_tears = after.dedicated_nui_tears;
      return true;
    }
  }
  return false;
}

std::optional<std::vector<CharacterItem>>
MemoryCharacterStateRepository::commit_item_trade(std::int64_t account_id,
                                                    std::int64_t character_id, int world_id,
                                                    const WorldProgress &before,
                                                    const WorldProgress &after,
                                                    const ItemTrade     &trade) {
  validate_transition(before, after);
  validate_item_trade(trade);

  std::lock_guard lock(database_->mutex);
  const auto owner =
      detail::find_owned_character(*database_, account_id, character_id, world_id);
  if (owner == database_->characters.end()) {
    return std::nullopt;
  }

  auto &stored = database_->world_progress[character_id];
  if (stored.revision != before.revision || stored.dead != before.dead) {
    return std::nullopt;
  }

  return apply_memory_trade(*database_, *owner, after, trade);
}

namespace {

WorldProgress read_postgres_world_progress(pqxx::transaction_base &transaction,
                                           std::int64_t account_id,
                                           std::int64_t character_id, int world_id) {
  const auto character = transaction.exec(
      "SELECT gameplay_revision,gold,hersander_token,experience,player_dead,level,"
      "level_up_points,weapon_points,nui_blessing_level,dedicated_nui_tears "
      "FROM emulator_characters WHERE account_id=$1 AND id=$2 AND world_id=$3",
      pqxx::params{account_id, character_id, world_id});
  if (character.empty()) {
    throw std::runtime_error("World progress character is not owned by this account");
  }
  WorldProgress state;
  state.revision = character.front()[0].as<std::int64_t>();
  state.gold = character.front()[1].as<std::int64_t>();
  state.hersander_token = character.front()[2].as<std::int64_t>();
  state.experience = character.front()[3].as<std::int64_t>();
  state.dead = character.front()[4].as<bool>();
  state.level = character.front()[5].as<int>();
  state.level_up_points = character.front()[6].as<int>();
  state.weapon_points = character.front()[7].as<int>();
  state.nui_blessing_level = character.front()[8].as<int>();
  state.dedicated_nui_tears = character.front()[9].as<std::int32_t>();
  state.build = detail::read_character_build(transaction, character_id);
  for (const auto &row : transaction.exec(
           "SELECT item_cms_id,quantity FROM emulator_character_stackables WHERE character_id=$1",
           pqxx::params{character_id})) {
    state.stackables.emplace(row[0].as<int>(), row[1].as<int>());
  }
  for (const auto &row :
       transaction.exec("SELECT object_cms_id,state FROM emulator_character_adventure_objects "
                        "WHERE character_id=$1",
                        pqxx::params{character_id})) {
    state.adventure_objects.emplace(row[0].as<int>(), row[1].as<int>());
  }
  for (const auto &row :
       transaction.exec("SELECT quest_cms_id,complete,success_count,objectives,credited_kills,"
                        "attempt,abandoned,started_epoch_ms "
                        "FROM emulator_character_quests "
                        "WHERE character_id=$1",
                        pqxx::params{character_id})) {
    QuestProgress quest;
    quest.complete = row[1].as<bool>();
    quest.success_count = row[2].as<int>();
    quest.attempt = row[5].as<int>();
    quest.abandoned = row[6].as<bool>();
    quest.started_epoch_ms = row[7].as<std::int64_t>();
    const auto objectives = nlohmann::json::parse(row[3].as<std::string>());
    for (const auto &[sequence, value] : objectives.items()) {
      quest.objectives.emplace(std::stoi(sequence), value.get<int>());
    }
    const auto kills = nlohmann::json::parse(row[4].as<std::string>());
    if (!kills.is_array() || kills.size() > 4096) {
      throw std::runtime_error("Invalid stored quest kill receipts");
    }
    for (const auto &unit_id : kills) {
      if (!quest.credited_kills.insert(unit_id.get<std::uint64_t>()).second) {
        throw std::runtime_error("Duplicate stored quest kill receipt");
      }
    }
    state.quests.emplace(row[0].as<int>(), std::move(quest));
  }
  for (const auto &row : transaction.exec(
           "SELECT unit_id,phase FROM emulator_character_doodad_phases WHERE character_id=$1",
           pqxx::params{character_id})) {
    state.doodad_phases.emplace(row[0].as<std::uint64_t>(), row[1].as<int>());
  }
  for (const auto &row : transaction.exec("SELECT slot,item_cms_id,quantity FROM "
                                          "emulator_character_utility_slots WHERE character_id=$1",
                                          pqxx::params{character_id})) {
    state.utility_slots.at(row[0].as<std::size_t>()) = {row[1].as<int>(), row[2].as<int>()};
  }
  for (const auto &row :
       transaction.exec("SELECT shop_id,item_cms_id,purchased_count,reset_epoch_ticks "
                        "FROM emulator_character_shop_purchases WHERE character_id=$1",
                        pqxx::params{character_id})) {
    const ShopLimitKey key{row[0].as<int>(), row[1].as<int>()};
    const ShopPurchaseRecord record{row[2].as<int>(), row[3].as<std::int64_t>()};
    state.shop_purchases.emplace(key, record);
  }
  for (const auto &row :
       transaction.exec("SELECT item_cms_id,quantity,installed_count "
                        "FROM emulator_character_housing_items WHERE character_id=$1",
                        pqxx::params{character_id})) {
    state.housing_items.emplace(row[0].as<int>(),
                                HousingItemRecord{row[1].as<int>(), row[2].as<int>()});
  }
  for (const auto &row :
       transaction.exec("SELECT voucher_id,good_cms_id,created_epoch_ticks,"
                        "expiration_epoch_ticks,redeemed "
                        "FROM emulator_character_trade_vouchers WHERE character_id=$1",
                        pqxx::params{character_id})) {
    state.trade_vouchers.emplace(
        row[0].as<std::int64_t>(),
        TradeVoucherRecord{row[1].as<int>(), row[2].as<std::int64_t>(), row[3].as<std::int64_t>(),
                           row[4].as<bool>()});
  }
  for (const auto &row : transaction.exec(
           "SELECT summon_cms_id FROM emulator_character_summons WHERE character_id=$1",
           pqxx::params{character_id})) {
    state.registered_summons.insert(row[0].as<int>());
  }
  for (const auto &row : transaction.exec(
           "SELECT npc_cms_id FROM emulator_character_loot_first_kills WHERE character_id=$1",
           pqxx::params{character_id})) {
    state.loot_first_kills.insert(row[0].as<int>());
  }
  for (const auto &row : transaction.exec(
           "SELECT emotion_cms_id FROM emulator_character_emotions WHERE character_id=$1",
           pqxx::params{character_id})) {
    state.owned_emotions.insert(row[0].as<int>());
  }
  for (const auto &row :
       transaction.exec("SELECT mate_cms_id,state,housing_lot_number,intimacy "
                        "FROM emulator_character_mates WHERE character_id=$1",
                        pqxx::params{character_id})) {
    state.mates.emplace(row[0].as<int>(),
                        MateRecord{row[1].as<int>(), row[2].as<int>(), row[3].as<int>()});
  }
  for (const auto &row :
       transaction.exec("SELECT craft_cms_id FROM emulator_character_crafts WHERE character_id=$1",
                        pqxx::params{character_id})) {
    state.learned_crafts.insert(row[0].as<int>());
  }
  for (const auto &row : transaction.exec(
           "SELECT achievement_cms_id,state,completion_tick FROM emulator_character_achievements "
           "WHERE character_id=$1",
           pqxx::params{character_id})) {
    state.achievements.emplace(row[0].as<int>(),
                               AchievementRecord{row[1].as<int>(), row[2].as<std::int64_t>()});
  }
  for (const auto &row : transaction.exec(
           "SELECT board_cms_id,period,refreshes FROM emulator_character_job_boards "
           "WHERE character_id=$1",
           pqxx::params{character_id})) {
    state.job_boards.emplace(row[0].as<int>(),
                             JobBoardRecord{row[1].as<std::int64_t>(), row[2].as<int>()});
  }
  for (const auto &row : transaction.exec(
           "SELECT play_record_cms_id,count FROM emulator_character_play_records "
           "WHERE character_id=$1",
           pqxx::params{character_id})) {
    state.play_records.emplace(row[0].as<int>(), row[1].as<std::int64_t>());
  }
  const auto trade_run = transaction.exec(
      "SELECT phase,start_post_id,parked_post_id,vehicle_cms_id,started_epoch_ticks,goods,"
      "wagon_out,wagon_map_id,wagon_x,wagon_y,wagon_z,wagon_yaw,wagon_health,"
      "last_wagon_spawn_epoch_ticks FROM emulator_character_trade_runs WHERE character_id=$1",
      pqxx::params{character_id});
  if (!trade_run.empty()) {
    const auto &row = trade_run.front();
    auto &run = state.trade_run;
    run.phase = static_cast<TradeRunPhase>(row[0].as<int>());
    run.start_post_id = row[1].as<int>();
    run.parked_post_id = row[2].as<int>();
    run.vehicle_cms_id = row[3].as<int>();
    run.started_epoch_ticks = row[4].as<std::int64_t>();
    run.goods = trade_goods_from_json(nlohmann::json::parse(row[5].as<std::string>()));
    if (row[6].as<bool>()) {
      run.wagon_position = WorldPosition{row[7].as<int>(), row[8].as<int>(), row[9].as<int>(),
                                         row[10].as<int>(), row[11].as<float>()};
    }
    run.wagon_health = row[12].as<int>();
    run.last_wagon_spawn_epoch_ticks = row[13].as<std::int64_t>();
  }
  validate(state);
  return state;
}

} // namespace

WorldProgress PostgresCharacterStateRepository::load_world_progress(std::int64_t account_id,
                                                                    std::int64_t character_id,
                                                                    int world_id) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  transaction.exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
  return read_postgres_world_progress(transaction, account_id, character_id, world_id);
}

std::optional<DeathReconciliationSnapshot>
PostgresCharacterStateRepository::fence_world_death_reconciliation(
    std::int64_t account_id, std::int64_t character_id, int world_id) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  transaction.exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
  transaction.exec("SET LOCAL lock_timeout = '5s'");
  transaction.exec("SET LOCAL statement_timeout = '15s'");
  const auto fenced = transaction.exec(
      "UPDATE emulator_characters SET gameplay_revision=gameplay_revision+1 "
      "WHERE account_id=$1 AND id=$2 AND world_id=$3 "
      "AND gameplay_revision<9223372036854775807 RETURNING id",
      pqxx::params{account_id, character_id, world_id});
  if (fenced.empty()) {
    return std::nullopt;
  }

  DeathReconciliationSnapshot snapshot;
  snapshot.progress = read_postgres_world_progress(transaction, account_id, character_id, world_id);
  const auto rows = transaction.exec(
      "SELECT id,item_cms_id,inventory_slot,equip_slot,quantity,durability,grade,"
      "created_epoch_ticks,polish_step FROM emulator_character_items "
      "WHERE character_id=$1 AND equip_slot IS NOT NULL ORDER BY inventory_slot LIMIT $2",
      pqxx::params{character_id, kEquipSlotCount + 1});
  if (rows.size() > static_cast<std::size_t>(kEquipSlotCount)) {
    throw std::runtime_error("Too many equipped items during death reconciliation");
  }
  for (const auto &row : rows) {
    CharacterItem item;
    item.id = row[0].as<std::int64_t>();
    item.cms_id = row[1].as<int>();
    item.inventory_slot = row[2].as<int>();
    item.equip_slot = row[3].as<int>();
    item.quantity = row[4].as<int>();
    item.durability = row[5].as<int>();
    item.grade = row[6].as<int>();
    item.created_epoch_ticks = row[7].as<std::int64_t>();
    item.polish_step = row[8].as<int>();
    validate_character_item(item);
    if (item.id <= 0) {
      throw std::runtime_error("Invalid equipped item during death reconciliation");
    }
    snapshot.equipped_items.push_back(std::move(item));
  }

  transaction.commit();
  return snapshot;
}

bool PostgresCharacterStateRepository::save_world_progress(std::int64_t account_id,
                                                           std::int64_t character_id,
                                                           int                world_id,
                                                           const WorldProgress &before,
                                                           const WorldProgress &after) {
  validate_transition(before, after);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  if (!write_world_progress(transaction, account_id, character_id, world_id, before, after)) {
    return false;
  }
  transaction.commit();
  return true;
}

std::optional<std::vector<CharacterItem>>
PostgresCharacterStateRepository::commit_item_trade(std::int64_t account_id,
                                                    std::int64_t character_id, int world_id,
                                                    const WorldProgress &before,
                                                    const WorldProgress &after,
                                                    const ItemTrade     &trade) {
  validate_transition(before, after);
  validate_item_trade(trade);

  try {
    pqxx::connection connection(connection_);
    pqxx::work transaction(connection);
    auto committed = apply_postgres_trade(transaction, account_id, character_id, world_id,
                                          before, after, trade);
    if (!committed) {
      return std::nullopt;
    }
    transaction.commit();
    return committed;
  } catch (const pqxx::unique_violation &) {
    // A concurrent writer took a chosen inventory slot; nothing was committed.
    return std::nullopt;
  }
}

bool MemoryCharacterStateRepository::enqueue_monster_rewards(
    const std::vector<MonsterRewardReceipt> &receipts) {
  return enqueue_monster_rewards_with_script(receipts, std::nullopt);
}

bool MemoryCharacterStateRepository::enqueue_monster_rewards_with_script(
    const std::vector<MonsterRewardReceipt> &receipts,
    const std::optional<BossScriptIntent> &intent) {
  validate_monster_reward_with_script(receipts, intent);
  auto ordered = receipts;
  std::ranges::sort(ordered, [](const auto &left, const auto &right) {
    return left.character_id < right.character_id;
  });
  const auto &event_id = intent ? intent->event_id : ordered.front().receipt_id;
  const int world_id = intent ? intent->world_id : ordered.front().world_id;
  std::lock_guard lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());

  const auto existing_batch = database_->monster_reward_batches.find(event_id);
  if (existing_batch != database_->monster_reward_batches.end() &&
      (existing_batch->second.recipients != ordered || existing_batch->second.boss_script != intent)) {
    return false;
  }
  const auto world_events = database_->monster_reward_event_counts.find(world_id);
  if (existing_batch == database_->monster_reward_batches.end() &&
      world_events != database_->monster_reward_event_counts.end() &&
      world_events->second >= kMaximumStoredMonsterRewardEvents) {
    return false;
  }
  for (const auto &receipt : ordered) {
    const auto owner = detail::find_owned_character(*database_, receipt.account_id,
                                                     receipt.character_id, receipt.world_id);
    const auto found = database_->monster_rewards.find({receipt.character_id, receipt.receipt_id});
    if (existing_batch != database_->monster_reward_batches.end()) {
      if (owner == database_->characters.end()) {
        const auto character = std::ranges::find_if(database_->characters, [&](const auto &entry) {
          return entry.id == receipt.character_id;
        });
        if (character != database_->characters.end() || found != database_->monster_rewards.end()) {
          return false;
        }
        continue;
      }
      if (found == database_->monster_rewards.end() || found->second.receipt != receipt) {
        return false;
      }
      continue;
    }
    if (owner == database_->characters.end() || found != database_->monster_rewards.end()) {
      return false;
    }
    std::size_t pending_count = 0;
    auto pending = database_->monster_rewards.lower_bound({receipt.character_id, {}});
    while (pending != database_->monster_rewards.end() && pending->first.first == receipt.character_id) {
      if (!pending->second.processed) {
        ++pending_count;
      }
      ++pending;
    }
    if (pending_count >= kMaximumPendingMonsterRewardsPerCharacter) {
      return false;
    }
  }
  if (existing_batch != database_->monster_reward_batches.end()) {
    return true;
  }

  std::vector<std::pair<std::int64_t, std::string>> new_keys;
  new_keys.reserve(ordered.size());
  for (const auto &receipt : ordered) {
    const auto key = std::pair{receipt.character_id, receipt.receipt_id};
    if (!database_->monster_rewards.contains(key)) {
      new_keys.push_back(key);
    }
  }
  std::size_t inserted_count = 0;
  auto next_sequence = database_->next_monster_reward_sequence;
  const bool new_batch = existing_batch == database_->monster_reward_batches.end();
  try {
    if (new_batch) {
      database_->monster_reward_batches.emplace(
          event_id, MemoryDatabase::StoredMonsterRewardBatch{ordered, false, intent, !intent,
                                                               !intent});
    }
    for (const auto &receipt : ordered) {
      const auto key = std::pair{receipt.character_id, receipt.receipt_id};
      if (database_->monster_rewards.contains(key)) {
        continue;
      }
      if (next_sequence == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Monster reward sequence exhausted");
      }
      database_->monster_rewards.emplace(
          key, MemoryDatabase::StoredMonsterReward{receipt, false, next_sequence++});
      ++inserted_count;
    }
    if (new_batch) {
      ++database_->monster_reward_event_counts[world_id];
    }
  } catch (...) {
    for (std::size_t index = 0; index < inserted_count; ++index) {
      database_->monster_rewards.erase(new_keys[index]);
    }
    if (new_batch) {
      database_->monster_reward_batches.erase(event_id);
    }
    throw;
  }
  database_->next_monster_reward_sequence = next_sequence;
  return true;
}

namespace {

void index_ready_boss_script(MemoryDatabase &database, const std::string &event_id,
                             const MemoryDatabase::StoredMonsterRewardBatch &event) {
  if (!event.boss_script || event.boss_script_published) {
    return;
  }
  auto [world, created] = database.pending_boss_script_ids.try_emplace(event.boss_script->world_id);
  try {
    world->second.insert(event_id);
  } catch (...) {
    if (created) {
      database.pending_boss_script_ids.erase(world);
    }
    throw;
  }
}

void remove_ready_boss_script(MemoryDatabase &database, const BossScriptIntent &intent) {
  const auto world = database.pending_boss_script_ids.find(intent.world_id);
  if (world == database.pending_boss_script_ids.end()) {
    return;
  }
  world->second.erase(intent.event_id);
  if (world->second.empty()) {
    database.pending_boss_script_ids.erase(world);
  }
}

} // namespace

std::vector<BossScriptIntent> MemoryCharacterStateRepository::pending_boss_scripts(
    int world_id, const std::optional<std::string> &after, std::size_t limit) {
  if (world_id <= 0 || limit == 0 || limit > kMonsterRewardRecoveryPageSize ||
      (after && !valid_monster_reward_id(*after))) {
    throw std::invalid_argument("Invalid boss script recovery page");
  }
  std::lock_guard lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  std::vector<BossScriptIntent> result;
  result.reserve(limit);
  const auto world = database_->pending_boss_script_ids.find(world_id);
  if (world == database_->pending_boss_script_ids.end()) {
    return result;
  }
  auto id = after ? world->second.upper_bound(*after) : world->second.begin();
  for (; id != world->second.end(); ++id) {
    const auto event = database_->monster_reward_batches.find(*id);
    if (event == database_->monster_reward_batches.end() || !event->second.ready ||
        event->second.boss_script_published || !event->second.boss_script ||
        event->second.boss_script->world_id != world_id ||
        event->second.boss_script->event_id != *id) {
      throw std::runtime_error("Pending boss script index is inconsistent");
    }
    result.push_back(*event->second.boss_script);
    if (result.size() == limit) {
      break;
    }
  }
  return result;
}

BossScriptSettlement MemoryCharacterStateRepository::settle_shared_boss_script(
    const BossScriptIntent &intent) {
  const auto targets = boss_script_targets(intent);
  std::lock_guard lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  const auto event = database_->monster_reward_batches.find(intent.event_id);
  if (event == database_->monster_reward_batches.end() || !event->second.ready ||
      event->second.boss_script != intent) {
    return {};
  }
  using SourceKey = std::tuple<int, int, int, std::uint32_t, std::uint32_t>;
  std::vector<SourceKey> keys;
  keys.reserve(targets.size());
  std::size_t missing = 0;
  for (const auto &target : targets) {
    keys.emplace_back(intent.world_id, target.key.map_id, target.key.kind,
                      target.key.sector_id, target.key.local_index);
    const auto found = database_->gathering_sources.find(keys.back());
    if (found == database_->gathering_sources.end()) {
      if (event->second.boss_script_applied) {
        throw std::runtime_error("Settled boss script lost a source");
      }
      ++missing;
      continue;
    }
    if (found->second.pending_receipt_id) {
      return {};
    }
    if (found->second.generation >= std::numeric_limits<std::int64_t>::max() - 1) {
      throw std::overflow_error("Boss script source generation exhausted");
    }
  }
  auto [capacity, created] = database_->gathering_source_counts.try_emplace(intent.world_id, 0);
  static_cast<void>(created);
  if (capacity->second > kMaximumSharedGatheringSourcesPerWorld - missing) {
    return {};
  }
  std::vector<SourceKey> inserted;
  std::vector<SharedGatheringSourceRecord> result;
  std::string event_id = intent.event_id;
  inserted.reserve(missing);
  result.reserve(targets.size());
  try {
    for (std::size_t index = 0; index < targets.size(); ++index) {
      auto found = database_->gathering_sources.find(keys[index]);
      if (found == database_->gathering_sources.end()) {
        found = database_->gathering_sources.emplace(
            keys[index], StoredSharedGatheringSource{0, true, targets[index].phase,
                                                     std::nullopt, std::nullopt, std::nullopt,
                                                     std::nullopt}).first;
        inserted.push_back(keys[index]);
      }
      auto snapshot = found->second;
      ++snapshot.generation;
      result.push_back({targets[index].key, std::move(snapshot)});
    }
  } catch (...) {
    for (const auto &key : inserted) {
      database_->gathering_sources.erase(key);
    }
    throw;
  }
  for (const auto &key : keys) {
    ++database_->gathering_sources.find(key)->second.generation;
  }
  capacity->second += missing;
  event->second.boss_script_applied = true;
  return {true, std::move(result), std::move(event_id)};
}

bool MemoryCharacterStateRepository::mark_boss_script_published(const BossScriptIntent &intent) {
  validate_boss_script_intent(intent);
  if (intent.copy_scope != "shared") {
    throw std::invalid_argument("Private boss scripts require copy-local acknowledgement");
  }
  std::lock_guard lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  const auto event = database_->monster_reward_batches.find(intent.event_id);
  if (event == database_->monster_reward_batches.end() || !event->second.ready ||
      !event->second.boss_script_applied || event->second.boss_script != intent) {
    return false;
  }
  event->second.boss_script_published = true;
  remove_ready_boss_script(*database_, intent);
  return true;
}

bool MemoryCharacterStateRepository::mark_private_boss_script_applied(
    const BossScriptIntent &intent) {
  validate_boss_script_intent(intent);
  if (intent.copy_scope == "shared") {
    throw std::invalid_argument("Shared boss scripts require durable source settlement");
  }
  std::lock_guard lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  const auto event = database_->monster_reward_batches.find(intent.event_id);
  if (event == database_->monster_reward_batches.end() || !event->second.ready ||
      event->second.boss_script != intent) {
    return false;
  }
  event->second.boss_script_applied = true;
  event->second.boss_script_published = true;
  remove_ready_boss_script(*database_, intent);
  return true;
}

bool MemoryCharacterStateRepository::retire_private_boss_script(const BossScriptIntent &intent) {
  return mark_private_boss_script_applied(intent);
}

std::int64_t MemoryCharacterStateRepository::acquire_monster_reward_owner_epoch() {
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  if (database_->monster_reward_owner_epoch == std::numeric_limits<std::int64_t>::max()) {
    throw std::overflow_error("Monster reward owner epoch exhausted");
  }
  const auto epoch = ++database_->monster_reward_owner_epoch;
  database_->quest_services->owner_epoch = epoch;
  monster_reward_owner_epoch_.store(epoch);
  return epoch;
}

void MemoryCharacterStateRepository::adopt_monster_reward_owner_epoch(std::int64_t epoch) {
  if (epoch <= 0) {
    throw std::invalid_argument("Invalid monster reward owner epoch");
  }
  monster_reward_owner_epoch_.store(epoch);
}

bool MemoryCharacterStateRepository::mark_monster_reward_ready(const std::string &receipt_id) {
  if (!valid_monster_reward_id(receipt_id)) {
    throw std::invalid_argument("Invalid monster reward receipt ID");
  }
  std::lock_guard lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  const auto event = database_->monster_reward_batches.find(receipt_id);
  if (event == database_->monster_reward_batches.end()) {
    return false;
  }
  index_ready_boss_script(*database_, receipt_id, event->second);
  event->second.ready = true;
  return true;
}

std::size_t MemoryCharacterStateRepository::recover_staged_monster_rewards(std::size_t limit) {
  if (limit == 0 || limit > kMonsterRewardRecoveryPageSize) {
    throw std::invalid_argument("Invalid monster reward recovery page size");
  }
  std::lock_guard lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  std::size_t recovered = 0;
  for (auto &[id, event] : database_->monster_reward_batches) {
    if (!event.ready) {
      index_ready_boss_script(*database_, id, event);
      event.ready = true;
      ++recovered;
      if (recovered == limit) {
        break;
      }
    }
  }
  return recovered;
}

bool MemoryCharacterStateRepository::has_staged_monster_rewards() {
  std::lock_guard lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  return std::ranges::any_of(database_->monster_reward_batches, [](const auto &batch) {
    return !batch.second.ready;
  });
}

std::vector<MonsterRewardReceipt> MemoryCharacterStateRepository::pending_monster_rewards(
    std::int64_t account_id, std::int64_t character_id, int world_id) {
  std::lock_guard lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  if (detail::find_owned_character(*database_, account_id, character_id, world_id) ==
      database_->characters.end()) {
    throw std::runtime_error("Monster reward character is not owned by this account");
  }

  std::vector<std::pair<std::uint64_t, MonsterRewardReceipt>> ordered;
  std::optional<std::uint64_t> first_staged_sequence;
  for (const auto &[key, stored] : database_->monster_rewards) {
    if (key.first == character_id && !stored.processed) {
      const auto event = database_->monster_reward_batches.find(key.second);
      if (event == database_->monster_reward_batches.end()) {
        throw std::runtime_error("Monster reward receipt has no event");
      }
      if (!event->second.ready || !event->second.boss_script_published) {
        if (!first_staged_sequence || stored.enqueue_sequence < *first_staged_sequence) {
          first_staged_sequence = stored.enqueue_sequence;
        }
        continue;
      }
      ordered.emplace_back(stored.enqueue_sequence, stored.receipt);
    }
  }
  std::ranges::sort(ordered, [](const auto &left, const auto &right) {
    return left.first < right.first;
  });
  std::vector<MonsterRewardReceipt> pending;
  pending.reserve(ordered.size());
  for (const auto &entry : ordered) {
    if (pending.size() == kMonsterRewardPageSize ||
        (first_staged_sequence && entry.first > *first_staged_sequence)) {
      break;
    }
    pending.push_back(entry.second);
  }
  return pending;
}

MonsterRewardCommitResult MemoryCharacterStateRepository::commit_monster_reward(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &receipt_id,
    const WorldProgress &before, const std::optional<WorldProgress> &after, const ItemTrade &trade) {
  validate_item_trade(trade);
  if (!valid_monster_reward_id(receipt_id)) {
    throw std::invalid_argument("Invalid monster reward receipt ID");
  }
  if (after) {
    validate_transition(before, *after);
  } else if (!trade.granted_items.empty() || !trade.removed_item_ids.empty() ||
             !trade.durability_updates.empty()) {
    throw std::invalid_argument("Item changes need a monster reward progress transition");
  }

  std::lock_guard lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  const auto owner = detail::find_owned_character(*database_, account_id, character_id, world_id);
  const auto receipt = database_->monster_rewards.find({character_id, receipt_id});
  if (owner == database_->characters.end() || receipt == database_->monster_rewards.end() ||
      receipt->second.receipt.account_id != account_id ||
      receipt->second.receipt.world_id != world_id) {
    return {};
  }
  if (receipt->second.processed) {
    return {MonsterRewardCommitStatus::already_processed, {}};
  }
  const auto event = database_->monster_reward_batches.find(receipt_id);
  if (event == database_->monster_reward_batches.end() || !event->second.ready ||
      !event->second.boss_script_published) {
    return {};
  }
  for (const auto &[key, other] : database_->monster_rewards) {
    if (key.first != character_id || other.processed ||
        other.enqueue_sequence >= receipt->second.enqueue_sequence) {
      continue;
    }
    const auto earlier = database_->monster_reward_batches.find(key.second);
    if (earlier == database_->monster_reward_batches.end() || !earlier->second.ready ||
        !earlier->second.boss_script_published) {
      return {};
    }
  }
  const auto &stored = database_->world_progress[character_id];
  if (stored.revision != before.revision || stored.dead != before.dead) {
    return {};
  }

  std::vector<CharacterItem> items;
  if (after) {
    auto committed = apply_memory_trade(*database_, *owner, *after, trade);
    if (!committed) {
      return {};
    }
    items = std::move(*committed);
  }
  receipt->second.processed = true;
  return {MonsterRewardCommitStatus::committed, std::move(items)};
}

namespace {

void swap_prepared_build(CharacterBuild &left, CharacterBuild &right) noexcept {
  using std::swap;
  swap(left.active_weapon_slot, right.active_weapon_slot);
  left.stat_bonuses.swap(right.stat_bonuses);
  left.perks.swap(right.perks);
}

void swap_prepared_trade_run(TradeRunRecord &left, TradeRunRecord &right) noexcept {
  static_assert(std::is_nothrow_swappable_v<std::optional<WorldPosition>>);
  using std::swap;
  swap(left.phase, right.phase);
  swap(left.start_post_id, right.start_post_id);
  swap(left.parked_post_id, right.parked_post_id);
  swap(left.vehicle_cms_id, right.vehicle_cms_id);
  swap(left.started_epoch_ticks, right.started_epoch_ticks);
  left.goods.swap(right.goods);
  swap(left.wagon_position, right.wagon_position);
  swap(left.wagon_health, right.wagon_health);
  swap(left.last_wagon_spawn_epoch_ticks, right.last_wagon_spawn_epoch_ticks);
}

// WorldProgress's aggregate swap is not noexcept on MSVC. Its constituent container swaps
// are noexcept; keep this list in sync with the complete canonical mutation serialization.
void swap_prepared_progress(WorldProgress &left, WorldProgress &right) noexcept {
  static_assert(std::is_nothrow_swappable_v<std::array<UtilitySlot, 4>>);
  static_assert(std::is_nothrow_swappable_v<std::map<int, QuestProgress>>);
  static_assert(std::is_nothrow_swappable_v<std::map<ShopLimitKey, ShopPurchaseRecord>>);
  using std::swap;
  swap(left.revision, right.revision);
  swap(left.dead, right.dead);
  swap(left.gold, right.gold);
  swap(left.hersander_token, right.hersander_token);
  swap(left.experience, right.experience);
  swap(left.level, right.level);
  swap(left.level_up_points, right.level_up_points);
  swap(left.weapon_points, right.weapon_points);
  swap(left.nui_blessing_level, right.nui_blessing_level);
  swap(left.dedicated_nui_tears, right.dedicated_nui_tears);
  swap_prepared_build(left.build, right.build);
  left.stackables.swap(right.stackables);
  left.adventure_objects.swap(right.adventure_objects);
  left.quests.swap(right.quests);
  left.doodad_phases.swap(right.doodad_phases);
  swap(left.utility_slots, right.utility_slots);
  left.shop_purchases.swap(right.shop_purchases);
  left.housing_items.swap(right.housing_items);
  left.trade_vouchers.swap(right.trade_vouchers);
  left.registered_summons.swap(right.registered_summons);
  left.mates.swap(right.mates);
  swap_prepared_trade_run(left.trade_run, right.trade_run);
  left.loot_first_kills.swap(right.loot_first_kills);
  left.owned_emotions.swap(right.owned_emotions);
  left.learned_crafts.swap(right.learned_crafts);
  left.achievements.swap(right.achievements);
  left.job_boards.swap(right.job_boards);
  left.play_records.swap(right.play_records);
}

void swap_prepared_character_changes(Character &left, Character &right) noexcept {
  using std::swap;
  left.items.swap(right.items);
  swap(left.experience, right.experience);
  swap(left.level, right.level);
  swap_prepared_build(left.build, right.build);
  swap(left.nui_blessing_level, right.nui_blessing_level);
  swap(left.dedicated_nui_tears, right.dedicated_nui_tears);
}

// The coordinator and database mutexes are both held, in that order. Prepare every allocation
// before inserting the event or publishing the new character/progress snapshots.
QuestMutationResult commit_memory_quest_mutation(
    MemoryDatabase &database, std::int64_t owner_epoch, std::int64_t account_id,
    std::int64_t character_id, int world_id, const std::string &effect_id,
    const WorldProgress &before, const std::optional<WorldProgress> &after,
    const ItemTrade &trade, const QuestServiceEffects &effects, bool monster_receipt,
    const std::optional<std::string> &combat_source = std::nullopt) {
  require_memory_reward_owner(database, owner_epoch);
  auto &coordinator = *database.quest_services;
  const auto fingerprint = combat_source
      ? combat_quest_event_fingerprint(*combat_source, before, after, trade, effects)
      : quest_mutation_fingerprint(before, after, trade, effects);
  const auto key = std::make_pair(character_id, effect_id);
  const auto existing = coordinator.events.find(key);
  if (existing != coordinator.events.end()) {
    if (existing->second.account_id != account_id || existing->second.world_id != world_id ||
        existing->second.mutation_fingerprint != fingerprint ||
        existing->second.monster_receipt != monster_receipt) {
      return {};
    }
    return {QuestMutationStatus::already_processed, existing->second.granted_items};
  }

  const auto owner = detail::find_owned_character(database, account_id, character_id, world_id);
  if (owner == database.characters.end()) {
    return {};
  }
  auto receipt = database.monster_rewards.end();
  if (monster_receipt) {
    receipt = database.monster_rewards.find(key);
    if (receipt == database.monster_rewards.end() ||
        receipt->second.receipt.account_id != account_id ||
        receipt->second.receipt.world_id != world_id) {
      return {};
    }
    // A receipt processed by an older build has no recoverable effect payload.
    if (receipt->second.processed) {
      return {QuestMutationStatus::already_processed, {}};
    }
    const auto batch = database.monster_reward_batches.find(effect_id);
    if (batch == database.monster_reward_batches.end() || !batch->second.ready ||
        !batch->second.boss_script_published) {
      return {};
    }
    for (const auto &[other_key, other] : database.monster_rewards) {
      if (other_key.first != character_id || other.processed ||
          other.enqueue_sequence >= receipt->second.enqueue_sequence) {
        continue;
      }
      const auto earlier = database.monster_reward_batches.find(other_key.second);
      if (earlier == database.monster_reward_batches.end() || !earlier->second.ready ||
          !earlier->second.boss_script_published) {
        return {};
      }
    }
  }

  const auto company = coordinator.company_memberships.find(character_id);
  const auto guild = coordinator.guild_memberships.find(character_id);
  const auto company_generation = coordinator.company_generations.find(character_id);
  const auto guild_generation = coordinator.guild_generations.find(character_id);
  const int company_type = company == coordinator.company_memberships.end() ? 0 : company->second;
  const std::int64_t guild_id = guild == coordinator.guild_memberships.end() ? 0 : guild->second;
  const auto current_company_generation = company_generation == coordinator.company_generations.end()
                                              ? 0 : company_generation->second;
  const auto current_guild_generation = guild_generation == coordinator.guild_generations.end()
                                            ? 0 : guild_generation->second;
  if (company_type != effects.expected_company_type || guild_id != effects.expected_guild_id ||
      current_company_generation != effects.expected_company_generation ||
      current_guild_generation != effects.expected_guild_generation ||
      coordinator.events.size() >= kMaximumStoredQuestServiceEvents ||
      coordinator.next_sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return {};
  }
  if (coordinator.pending_count(character_id) >= kMaximumPendingQuestServiceEffectsPerCharacter) {
    return {};
  }

  auto [progress_entry, inserted_progress] = database.world_progress.try_emplace(character_id);
  static_cast<void>(inserted_progress);
  if (progress_entry->second.revision != before.revision || progress_entry->second.dead != before.dead) {
    return {};
  }
  if (combat_source) {
    auto authoritative = progress_entry->second;
    authoritative.experience = owner->experience;
    authoritative.level = owner->level;
    authoritative.build = owner->build;
    authoritative.nui_blessing_level = owner->nui_blessing_level;
    authoritative.dedicated_nui_tears = owner->dedicated_nui_tears;
    if (authoritative != before) {
      return {};
    }
  }

  std::vector<CharacterItem> granted;
  std::optional<detail::PreparedMemoryLootTrade> prepared;
  if (after) {
    prepared = detail::prepare_memory_loot_trade(database, *owner, *after, trade);
    if (!prepared) {
      return {};
    }
    granted = prepared->granted_items;
  }

  QuestServiceEffectEvent event;
  event.account_id = account_id;
  event.character_id = character_id;
  event.world_id = world_id;
  event.effect_id = effect_id;
  event.mutation_fingerprint = fingerprint;
  event.monster_receipt = monster_receipt;
  event.effects = effects;
  event.granted_items = granted;
  event.company_applied = company_type == 0 ||
                          (effects.company_contribution == 0 && effects.company_unlocks.empty() &&
                           effects.activity_experience == 0);
  event.guild_applied = guild_id == 0 ||
                        (effects.guild_experience == 0 && effects.activity_experience == 0);
  if (!coordinator.record_event(std::move(event))) {
    return {};
  }
  if (after) {
    detail::publish_memory_loot_trade(database, *owner, progress_entry->second, *prepared);
  }
  if (monster_receipt) {
    receipt->second.processed = true;
  }
  return {QuestMutationStatus::committed, std::move(granted)};
}

void validate_quest_effect_request(const std::string &effect_id, const WorldProgress &before,
                                   const std::optional<WorldProgress> &after,
                                   const ItemTrade &trade, const QuestServiceEffects &effects) {
  if (!valid_monster_reward_id(effect_id)) {
    throw std::invalid_argument("Invalid quest service effect request");
  }
  static_cast<void>(encode_quest_service_effects(effects));
  validate_item_trade(trade);
  if (after) {
    validate_transition(before, *after);
  } else if (!trade.granted_items.empty() || !trade.removed_item_ids.empty() ||
             !trade.durability_updates.empty()) {
    throw std::invalid_argument("Item changes need a quest progress transition");
  }
}

} // namespace

namespace detail {

void validate_loot_trade(const WorldProgress &before, const WorldProgress &after,
                         const ItemTrade &trade) {
  validate_transition(before, after);
  validate_item_trade(trade);
}

WorldProgress read_locked_world_progress(pqxx::transaction_base &transaction,
                                          std::int64_t account_id,
                                          std::int64_t character_id, int world_id) {
  return read_postgres_world_progress(transaction, account_id, character_id, world_id);
}

std::optional<PreparedMemoryLootTrade>
prepare_memory_loot_trade(const MemoryDatabase &database, const Character &owner,
                          const WorldProgress &after, const ItemTrade &trade) {
  PreparedMemoryLootTrade prepared{owner, after, {}, database.next_item_id};
  prepared.granted_items.reserve(trade.granted_items.size());
  for (const auto item_id : trade.removed_item_ids) {
    const auto item = std::ranges::find_if(prepared.owner.items, [item_id](const CharacterItem &entry) {
      return entry.id == item_id;
    });
    if (item == prepared.owner.items.end() || item->equip_slot) {
      return std::nullopt;
    }
    prepared.owner.items.erase(item);
  }
  for (const auto &change : trade.durability_updates) {
    const auto item = std::ranges::find_if(prepared.owner.items, [&](const CharacterItem &entry) {
      return entry.id == change.item_id;
    });
    if (item == prepared.owner.items.end() || item->durability != change.expected_durability) {
      return std::nullopt;
    }
    item->durability = change.durability;
  }
  for (auto item : trade.granted_items) {
    if (std::ranges::any_of(prepared.owner.items, [&](const CharacterItem &entry) {
          return entry.inventory_slot == item.inventory_slot;
        })) {
      return std::nullopt;
    }
    if (prepared.next_item_id == std::numeric_limits<std::int64_t>::max()) {
      throw std::overflow_error("Character item IDs are exhausted");
    }
    item.id = prepared.next_item_id++;
    prepared.owner.items.push_back(item);
    prepared.granted_items.push_back(item);
  }
  prepared.owner.experience = after.experience;
  prepared.owner.level = after.level;
  prepared.owner.build = after.build;
  prepared.owner.nui_blessing_level = after.nui_blessing_level;
  prepared.owner.dedicated_nui_tears = after.dedicated_nui_tears;
  return prepared;
}

void publish_memory_loot_trade(MemoryDatabase &database, Character &owner,
                               WorldProgress &stored_progress,
                               PreparedMemoryLootTrade &prepared) noexcept {
  swap_prepared_character_changes(owner, prepared.owner);
  swap_prepared_progress(stored_progress, prepared.progress);
  database.next_item_id = prepared.next_item_id;
}

std::optional<std::vector<CharacterItem>>
apply_postgres_loot_trade(pqxx::work &transaction, std::int64_t account_id,
                          std::int64_t character_id, int world_id,
                          const WorldProgress &before, const WorldProgress &after,
                          const ItemTrade &trade) {
  return apply_postgres_trade(transaction, account_id, character_id, world_id, before, after,
                              trade);
}

} // namespace detail

LootClaimResult MemoryCharacterStateRepository::commit_gathering_quest_claim(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &receipt_id, const std::string &source_fingerprint,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
    const QuestServiceEffects &effects,
    const std::optional<SharedGatheringSourceTransition> &source) {
  const bool valid_fingerprint = source_fingerprint.size() == 64 &&
      std::ranges::all_of(source_fingerprint, [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
      });
  if (account_id <= 0 || character_id <= 0 || world_id <= 0 ||
      !valid_monster_reward_id(receipt_id) || !valid_fingerprint) {
    throw std::invalid_argument("Invalid gathering claim identity");
  }
  validate_quest_effect_request(receipt_id, before, after, trade, effects);
  if (source) {
    validate_shared_gathering_source(*source);
  }
  const auto fingerprint = gathering_mutation_fingerprint(source_fingerprint, before, after,
                                                            trade, effects, source);
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  const auto owner = detail::find_owned_character(*database_, account_id, character_id, world_id);
  if (owner == database_->characters.end()) {
    return {LootClaimStatus::conflict, {}};
  }
  const auto existing = database_->loot_claims.find(receipt_id);
  if (existing != database_->loot_claims.end()) {
    const auto &claim = existing->second;
    if (claim.claim_kind != "gathering" || claim.world_id != world_id ||
        claim.corpse_fingerprint != source_fingerprint) {
      return {LootClaimStatus::mismatch, {}};
    }
    if (claim.account_id != account_id || claim.character_id != character_id) {
      return {LootClaimStatus::claimed_by_other_character, {}};
    }
    if (claim.mutation_fingerprint != fingerprint) {
      return {LootClaimStatus::mismatch, {}};
    }
    return {LootClaimStatus::already_claimed_by_same_character, claim.granted_items};
  }
  const auto &coordinator = *database_->quest_services;
  const auto company = coordinator.company_memberships.find(character_id);
  const auto guild = coordinator.guild_memberships.find(character_id);
  const auto company_generation = coordinator.company_generations.find(character_id);
  const auto guild_generation = coordinator.guild_generations.find(character_id);
  const bool membership_changed =
      (company == coordinator.company_memberships.end() ? 0 : company->second) !=
          effects.expected_company_type ||
      (guild == coordinator.guild_memberships.end() ? 0 : guild->second) !=
          effects.expected_guild_id ||
      (company_generation == coordinator.company_generations.end() ? 0 : company_generation->second) !=
          effects.expected_company_generation ||
      (guild_generation == coordinator.guild_generations.end() ? 0 : guild_generation->second) !=
          effects.expected_guild_generation;
  if (coordinator.events.contains({character_id, receipt_id})) {
    return {LootClaimStatus::mismatch, {}};
  }
  // A revision/slot conflict must not hide a newer shared source state from the caller.
  if (source) {
    const auto key = std::make_tuple(world_id, source->map_id, source->kind,
                                     source->sector_id, source->local_index);
    const auto found = database_->gathering_sources.find(key);
    if (found == database_->gathering_sources.end()) {
      if (source->expected_generation != 0) {
        return {LootClaimStatus::source_conflict, {}, std::nullopt};
      }
    } else if (found->second.generation != source->expected_generation ||
               found->second.active != source->expected_active ||
               found->second.phase != source->expected_phase || found->second.pending_receipt_id) {
      return {LootClaimStatus::source_conflict, {}, found->second};
    }
    if (source->linked) {
      const auto &linked = *source->linked;
      const auto linked_key = std::make_tuple(world_id, linked.map_id, linked.kind,
                                              linked.sector_id, linked.local_index);
      const auto linked_found = database_->gathering_sources.find(linked_key);
      if (linked_found == database_->gathering_sources.end()) {
        if (linked.expected_generation != 0) {
          return {LootClaimStatus::source_conflict, {}, std::nullopt};
        }
      } else if (linked_found->second.generation != linked.expected_generation ||
                 linked_found->second.active != linked.expected_active ||
                 linked_found->second.phase != linked.expected_phase ||
                 linked_found->second.pending_receipt_id ||
                 linked_found->second.next_transition_epoch_ticks ||
                 linked_found->second.timed_successor_phase ||
                 linked_found->second.next_transition_delay_ticks) {
        return {LootClaimStatus::source_conflict, {}, linked_found->second};
      }
    }
  }
  if (membership_changed) {
    return {LootClaimStatus::conflict, {}};
  }
  const auto progress = database_->world_progress.find(character_id);
  const WorldProgress empty_progress;
  const auto &current = progress == database_->world_progress.end() ? empty_progress : progress->second;
  if (current.revision != before.revision || current.dead != before.dead) {
    return {LootClaimStatus::conflict, {}};
  }
  auto prepared = detail::prepare_memory_loot_trade(*database_, *owner, after, trade);
  if (!prepared) {
    return {LootClaimStatus::conflict, {}};
  }
  auto [capacity, new_capacity] = database_->loot_claim_counts.try_emplace(world_id, 0);
  static_cast<void>(new_capacity);
  if (capacity->second >= kMaximumRetainedLootClaimsPerWorld) {
    return {LootClaimStatus::conflict, {}};
  }
  using SourceKey = std::tuple<int, int, int, std::uint32_t, std::uint32_t>;
  std::optional<SourceKey> source_key;
  std::optional<SourceKey> linked_key;
  bool inserted_source = false;
  bool inserted_linked = false;
  StoredSharedGatheringSource *source_to_publish = nullptr;
  StoredSharedGatheringSource *linked_to_publish = nullptr;
  std::size_t *source_count = nullptr;
  if (source) {
    source_key.emplace(world_id, source->map_id, source->kind, source->sector_id,
                       source->local_index);
    auto existing_source = database_->gathering_sources.find(*source_key);
    if (source->linked) {
      const auto &linked = *source->linked;
      linked_key.emplace(world_id, linked.map_id, linked.kind, linked.sector_id,
                         linked.local_index);
    }
    const bool missing_primary = existing_source == database_->gathering_sources.end();
    const bool missing_linked = linked_key && !database_->gathering_sources.contains(*linked_key);
    auto [capacity_entry, created] = database_->gathering_source_counts.try_emplace(world_id, 0);
    static_cast<void>(created);
    source_count = &capacity_entry->second;
    const auto needed = static_cast<std::size_t>(missing_primary) +
                        static_cast<std::size_t>(missing_linked);
    if (*source_count > kMaximumSharedGatheringSourcesPerWorld - needed) {
      return {LootClaimStatus::conflict, {}};
    }
    try {
      if (missing_primary) {
        existing_source = database_->gathering_sources.emplace(
            *source_key, StoredSharedGatheringSource{0, source->expected_active,
                                                     source->expected_phase, std::nullopt,
                                                     std::nullopt, std::nullopt,
                                                     std::nullopt}).first;
        inserted_source = true;
      }
      source_to_publish = &existing_source->second;
      if (linked_key) {
        auto existing_linked = database_->gathering_sources.find(*linked_key);
        if (existing_linked == database_->gathering_sources.end()) {
          const auto &linked = *source->linked;
          existing_linked = database_->gathering_sources.emplace(
              *linked_key, StoredSharedGatheringSource{0, linked.expected_active,
                                                       linked.expected_phase, std::nullopt,
                                                       std::nullopt, std::nullopt,
                                                       std::nullopt}).first;
          inserted_linked = true;
        }
        linked_to_publish = &existing_linked->second;
      }
    } catch (...) {
      if (inserted_source) {
        database_->gathering_sources.erase(*source_key);
      }
      throw;
    }
  }

  const bool has_effect =
      (effects.expected_company_type != 0 &&
       (effects.company_contribution != 0 || effects.activity_experience != 0 ||
        !effects.company_unlocks.empty())) ||
      (effects.expected_guild_id != 0 &&
       (effects.guild_experience != 0 || effects.activity_experience != 0));
  try {
    LootClaimResult result{LootClaimStatus::committed, prepared->granted_items};
    std::optional<std::string> pending_receipt;
    if (source) {
      pending_receipt = receipt_id;
    }
    StoredLootClaim claim{account_id, character_id, world_id, source_fingerprint,
                          prepared->granted_items, "gathering", fingerprint, false,
                          std::nullopt, source.has_value()};
    const auto [receipt, inserted] = database_->loot_claims.emplace(receipt_id, std::move(claim));
    if (!inserted) {
      throw std::logic_error("Gathering receipt changed under the database lock");
    }
    try {
      if (has_effect) {
        const auto saved = commit_memory_quest_mutation(
            *database_, monster_reward_owner_epoch_.load(), account_id, character_id, world_id,
            receipt_id, before, after, trade, effects, false);
        if (saved.status != QuestMutationStatus::committed) {
          database_->loot_claims.erase(receipt);
          if (inserted_source) {
            database_->gathering_sources.erase(*source_key);
          }
          if (inserted_linked) {
            database_->gathering_sources.erase(*linked_key);
          }
          return {LootClaimStatus::conflict, {}};
        }
      } else {
        auto [stored_progress, inserted] = database_->world_progress.try_emplace(character_id);
        static_cast<void>(inserted);
        detail::publish_memory_loot_trade(*database_, *owner, stored_progress->second, *prepared);
      }
      if (source) {
        auto &stored_source = *source_to_publish;
        stored_source.generation = source->expected_generation + 1;
        stored_source.active = source->resulting_active;
        stored_source.phase = source->resulting_phase;
        stored_source.next_transition_epoch_ticks = std::nullopt;
        stored_source.timed_successor_phase = source->timed_successor_phase;
        stored_source.pending_receipt_id.swap(pending_receipt);
        stored_source.next_transition_delay_ticks = source->next_transition_delay_ticks;
        if (linked_to_publish) {
          linked_to_publish->generation = source->linked->expected_generation + 1;
          linked_to_publish->active = false;
          linked_to_publish->phase = 0;
          linked_to_publish->next_transition_epoch_ticks.reset();
          linked_to_publish->timed_successor_phase.reset();
        }
      }
      ++capacity->second;
      if (source_count) {
        *source_count += static_cast<std::size_t>(inserted_source) +
                         static_cast<std::size_t>(inserted_linked);
      }
      return result;
    } catch (...) {
      database_->loot_claims.erase(receipt_id);
      throw;
    }
  } catch (...) {
    if (inserted_source) {
      database_->gathering_sources.erase(*source_key);
    }
    if (inserted_linked) {
      database_->gathering_sources.erase(*linked_key);
    }
    throw;
  }
}

GatheringFinalizeResult MemoryCharacterStateRepository::finalize_gathering_quest_claim(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &receipt_id, const std::string &source_fingerprint,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
    const QuestServiceEffects &effects,
    const std::optional<SharedGatheringSourceTransition> &source,
    std::int64_t confirmation_epoch_ticks) {
  if (confirmation_epoch_ticks <= 0) {
    throw std::invalid_argument("Invalid gathering confirmation time");
  }
  if (source) {
    validate_shared_gathering_source(*source);
  }
  const auto fingerprint = gathering_mutation_fingerprint(source_fingerprint, before, after,
                                                            trade, effects, source);
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  if (detail::find_owned_character(*database_, account_id, character_id, world_id) ==
      database_->characters.end()) {
    return {};
  }
  const auto receipt = database_->loot_claims.find(receipt_id);
  if (receipt == database_->loot_claims.end() || receipt->second.claim_kind != "gathering" ||
      receipt->second.account_id != account_id || receipt->second.character_id != character_id ||
      receipt->second.world_id != world_id || receipt->second.corpse_fingerprint != source_fingerprint ||
      receipt->second.mutation_fingerprint != fingerprint) {
    return {};
  }
  StoredSharedGatheringSource *linked_source = nullptr;
  if (source && source->linked) {
    const auto &linked = *source->linked;
    const auto key = std::make_tuple(world_id, linked.map_id, linked.kind,
                                     linked.sector_id, linked.local_index);
    const auto found = database_->gathering_sources.find(key);
    if (found == database_->gathering_sources.end() || found->second.pending_receipt_id ||
        found->second.active || found->second.phase != 0 ||
        found->second.next_transition_epoch_ticks || found->second.timed_successor_phase ||
        found->second.next_transition_delay_ticks) {
      return {};
    }
    if (found->second.generation >= std::numeric_limits<std::int64_t>::max() - 1) {
      throw std::overflow_error("Linked gathering source generation exhausted");
    }
    linked_source = &found->second;
  }
  if (receipt->second.terminal) {
    std::optional<StoredSharedGatheringSource> snapshot;
    if (source) {
      const auto key = std::make_tuple(world_id, source->map_id, source->kind,
                                       source->sector_id, source->local_index);
      const auto found = database_->gathering_sources.find(key);
      if (found == database_->gathering_sources.end()) {
        return {};
      }
      if (linked_source) {
        if (found->second.pending_receipt_id ||
            found->second.generation >= std::numeric_limits<std::int64_t>::max() - 1) {
          return {};
        }
        ++found->second.generation;
        ++linked_source->generation;
      }
      snapshot = found->second;
    }
    return {GatheringFinalizeStatus::already_finalized,
            receipt->second.armed_deadline_epoch_ticks, std::move(snapshot),
            linked_source ? std::optional<StoredSharedGatheringSource>{*linked_source}
                          : std::nullopt};
  }
  std::optional<std::int64_t> deadline;
  StoredSharedGatheringSource *stored_source = nullptr;
  if (source) {
    const auto key = std::make_tuple(world_id, source->map_id, source->kind, source->sector_id,
                                     source->local_index);
    const auto found = database_->gathering_sources.find(key);
    if (found == database_->gathering_sources.end() ||
        found->second.generation != source->expected_generation + 1 ||
        found->second.pending_receipt_id != receipt_id ||
        found->second.next_transition_delay_ticks != source->next_transition_delay_ticks) {
      return {};
    }
    stored_source = &found->second;
    if (source->next_transition_delay_ticks) {
      const auto delay = *source->next_transition_delay_ticks;
      if (confirmation_epoch_ticks > std::numeric_limits<std::int64_t>::max() - delay) {
        throw std::overflow_error("Gathering transition deadline overflow");
      }
      deadline = confirmation_epoch_ticks + delay;
    }
  }
  if (stored_source) {
    ++stored_source->generation;
    stored_source->next_transition_epoch_ticks = deadline;
    stored_source->next_transition_delay_ticks.reset();
    stored_source->pending_receipt_id.reset();
  }
  if (linked_source) {
    ++linked_source->generation;
  }
  receipt->second.armed_deadline_epoch_ticks = deadline;
  receipt->second.terminal = true;
  const auto snapshot = stored_source ? std::optional<StoredSharedGatheringSource>{*stored_source}
                                      : std::nullopt;
  return {GatheringFinalizeStatus::finalized, deadline, snapshot,
          linked_source ? std::optional<StoredSharedGatheringSource>{*linked_source}
                        : std::nullopt};
}

std::size_t MemoryCharacterStateRepository::recover_gathering_claims(
    std::size_t limit, std::int64_t confirmation_epoch_ticks) {
  if (limit == 0 || limit > kMonsterRewardRecoveryPageSize || confirmation_epoch_ticks <= 0) {
    throw std::invalid_argument("Invalid gathering startup recovery page");
  }
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  struct Recovery {
    StoredLootClaim *claim;
    StoredSharedGatheringSource *source;
    std::optional<std::int64_t> deadline;
  };
  std::vector<Recovery> page;
  page.reserve(limit);
  for (auto &[receipt_id, claim] : database_->loot_claims) {
    if (claim.claim_kind != "gathering" || claim.terminal) {
      continue;
    }
    StoredSharedGatheringSource *source = nullptr;
    for (auto &[key, candidate] : database_->gathering_sources) {
      if (candidate.pending_receipt_id == receipt_id) {
        if (source || std::get<0>(key) != claim.world_id) {
          throw std::runtime_error("Gathering receipt refers to multiple sources");
        }
        source = &candidate;
      }
    }
    if (claim.shared_source != static_cast<bool>(source)) {
      throw std::runtime_error("Gathering receipt lost its shared source");
    }
    std::optional<std::int64_t> deadline;
    if (source && source->next_transition_delay_ticks) {
      const auto delay = *source->next_transition_delay_ticks;
      if (delay <= 0 || source->generation == std::numeric_limits<std::int64_t>::max() ||
          confirmation_epoch_ticks > std::numeric_limits<std::int64_t>::max() - delay) {
        throw std::overflow_error("Gathering startup deadline cannot be armed");
      }
      deadline = confirmation_epoch_ticks + delay;
    }
    if (source && source->generation == std::numeric_limits<std::int64_t>::max()) {
      throw std::overflow_error("Gathering source generation exhausted");
    }
    page.push_back({&claim, source, deadline});
    if (page.size() == limit) {
      break;
    }
  }
  // All allocations and validation precede this nonthrowing publication.
  for (const auto &entry : page) {
    if (entry.source) {
      ++entry.source->generation;
      entry.source->next_transition_epoch_ticks = entry.deadline;
      entry.source->next_transition_delay_ticks.reset();
      entry.source->pending_receipt_id.reset();
    }
    entry.claim->armed_deadline_epoch_ticks = entry.deadline;
    entry.claim->terminal = true;
  }
  return page.size();
}

bool MemoryCharacterStateRepository::has_unfinished_gathering_claims() {
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  return std::ranges::any_of(database_->loot_claims, [](const auto &entry) {
    return entry.second.claim_kind == "gathering" && !entry.second.terminal;
  });
}

std::vector<SharedGatheringSourceRecord> MemoryCharacterStateRepository::shared_gathering_sources(
    int world_id, const std::optional<SharedGatheringSourceKey> &after, std::size_t limit) {
  if (world_id <= 0 || limit == 0 || limit > kSharedGatheringSourcePageSize ||
      (after && (after->map_id <= 0 || (after->kind != 0 && after->kind != 1)))) {
    throw std::invalid_argument("Invalid shared gathering hydration page");
  }
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  std::vector<SharedGatheringSourceRecord> result;
  result.reserve(limit);
  auto entry = after
      ? database_->gathering_sources.upper_bound({world_id, after->map_id, after->kind,
                                                  after->sector_id, after->local_index})
      : database_->gathering_sources.lower_bound({world_id, 0, 0, 0, 0});
  for (; entry != database_->gathering_sources.end() && std::get<0>(entry->first) == world_id;
       ++entry) {
    const auto &[world, map, kind, sector, local] = entry->first;
    static_cast<void>(world);
    result.push_back({{map, kind, sector, local}, entry->second});
    if (result.size() == limit) {
      break;
    }
  }
  return result;
}

SharedGatheringSourceUpdateResult MemoryCharacterStateRepository::update_shared_gathering_source(
    int world_id, const SharedGatheringSourceTransition &transition,
    std::int64_t epoch_ticks, bool require_due) {
  if (world_id <= 0 || epoch_ticks <= 0 || transition.linked) {
    throw std::invalid_argument("Invalid shared gathering transition time");
  }
  validate_shared_gathering_source(transition);
  std::optional<std::int64_t> deadline;
  if (transition.next_transition_delay_ticks) {
    const auto delay = *transition.next_transition_delay_ticks;
    if (epoch_ticks > std::numeric_limits<std::int64_t>::max() - delay) {
      throw std::overflow_error("Shared gathering transition deadline overflow");
    }
    deadline = epoch_ticks + delay;
  }
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  const auto key = std::make_tuple(world_id, transition.map_id, transition.kind,
                                   transition.sector_id, transition.local_index);
  auto found = database_->gathering_sources.find(key);
  std::size_t *source_count = nullptr;
  if (found == database_->gathering_sources.end()) {
    if (require_due || transition.expected_generation != 0) {
      return {};
    }
    auto [capacity_entry, created] = database_->gathering_source_counts.try_emplace(world_id, 0);
    static_cast<void>(created);
    if (capacity_entry->second >= kMaximumSharedGatheringSourcesPerWorld) {
      return {};
    }
    source_count = &capacity_entry->second;
    found = database_->gathering_sources.emplace(key, StoredSharedGatheringSource{
        0, transition.expected_active, transition.expected_phase, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt}).first;
  }
  auto &state = found->second;
  if (state.generation != transition.expected_generation ||
      state.active != transition.expected_active || state.phase != transition.expected_phase ||
      state.pending_receipt_id ||
      (require_due && (!state.next_transition_epoch_ticks ||
                       *state.next_transition_epoch_ticks > epoch_ticks))) {
    return {false, state};
  }
  state.generation = transition.expected_generation + 1;
  state.active = transition.resulting_active;
  state.phase = transition.resulting_phase;
  state.next_transition_epoch_ticks = deadline;
  state.timed_successor_phase = transition.timed_successor_phase;
  if (source_count) {
    ++*source_count;
  }
  return {true, state};
}

SharedGatheringSourceUpdateResult MemoryCharacterStateRepository::fence_shared_gathering_source(
    int world_id, const SharedGatheringSourceTransition &initial) {
  if (world_id <= 0) {
    throw std::invalid_argument("Invalid shared gathering fence world");
  }
  validate_shared_gathering_source(initial);
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  const auto key = std::make_tuple(world_id, initial.map_id, initial.kind,
                                   initial.sector_id, initial.local_index);
  auto found = database_->gathering_sources.find(key);
  using SourceKey = std::tuple<int, int, int, std::uint32_t, std::uint32_t>;
  std::optional<SourceKey> linked_key;
  if (initial.linked) {
    const auto &linked = *initial.linked;
    linked_key.emplace(world_id, linked.map_id, linked.kind, linked.sector_id,
                       linked.local_index);
  }
  auto linked_found = linked_key ? database_->gathering_sources.find(*linked_key)
                                 : database_->gathering_sources.end();
  const bool missing_primary = found == database_->gathering_sources.end();
  const bool missing_linked = linked_key && linked_found == database_->gathering_sources.end();
  if ((missing_primary && initial.expected_generation != 0) ||
      (missing_linked && initial.linked->expected_generation != 0)) {
    return {};
  }
  if ((!missing_primary && found->second.pending_receipt_id) ||
      (!missing_linked && linked_key && linked_found->second.pending_receipt_id)) {
    return {false, missing_primary ? std::nullopt
                                   : std::optional<StoredSharedGatheringSource>{found->second},
            missing_linked || !linked_key ? std::nullopt
                : std::optional<StoredSharedGatheringSource>{linked_found->second}};
  }
  if (!missing_linked && linked_key &&
      (linked_found->second.active ||
       (linked_found->second.phase && linked_found->second.phase != 0) ||
       linked_found->second.next_transition_epoch_ticks ||
       linked_found->second.timed_successor_phase ||
       linked_found->second.next_transition_delay_ticks)) {
    return {false, missing_primary ? std::nullopt
                                   : std::optional<StoredSharedGatheringSource>{found->second},
            linked_found->second};
  }
  if ((!missing_primary && found->second.generation >= std::numeric_limits<std::int64_t>::max() - 1) ||
      (!missing_linked && linked_key &&
       linked_found->second.generation >= std::numeric_limits<std::int64_t>::max() - 1)) {
    throw std::overflow_error("Shared gathering source generation exhausted");
  }
  auto [capacity, created] = database_->gathering_source_counts.try_emplace(world_id, 0);
  static_cast<void>(created);
  const auto needed = static_cast<std::size_t>(missing_primary) +
                      static_cast<std::size_t>(missing_linked);
  if (capacity->second > kMaximumSharedGatheringSourcesPerWorld - needed) {
    return {};
  }
  bool inserted_primary = false;
  try {
    if (missing_primary) {
      found = database_->gathering_sources.emplace(
          key, StoredSharedGatheringSource{0, initial.expected_active, initial.expected_phase,
                                           std::nullopt, std::nullopt, std::nullopt,
                                           std::nullopt}).first;
      inserted_primary = true;
    }
    if (missing_linked) {
      const auto &linked = *initial.linked;
      linked_found = database_->gathering_sources.emplace(
          *linked_key, StoredSharedGatheringSource{0, linked.expected_active, linked.expected_phase,
                                                   std::nullopt, std::nullopt, std::nullopt,
                                                   std::nullopt}).first;
    }
  } catch (...) {
    if (inserted_primary) {
      database_->gathering_sources.erase(key);
    }
    throw;
  }
  ++found->second.generation;
  if (linked_key) {
    ++linked_found->second.generation;
  }
  capacity->second += needed;
  return {true, found->second,
          linked_key ? std::optional<StoredSharedGatheringSource>{linked_found->second}
                     : std::nullopt};
}

std::optional<std::vector<CharacterItem>>
MemoryCharacterStateRepository::commit_unaffiliated_activity(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const QuestServiceMembership &membership, const WorldProgress &before,
    const WorldProgress &after, const ItemTrade &trade) {
  if (account_id <= 0 || character_id <= 0 || world_id <= 0 ||
      membership.company_type != 0 || membership.guild_id != 0 ||
      membership.company_generation < 0 || membership.guild_generation < 0) {
    throw std::invalid_argument("Invalid unaffiliated activity identity");
  }
  detail::validate_loot_trade(before, after, trade);
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  const auto &coordinator = *database_->quest_services;
  const auto company = coordinator.company_memberships.find(character_id);
  const auto guild = coordinator.guild_memberships.find(character_id);
  const auto company_generation = coordinator.company_generations.find(character_id);
  const auto guild_generation = coordinator.guild_generations.find(character_id);
  if ((company != coordinator.company_memberships.end() && company->second != 0) ||
      (guild != coordinator.guild_memberships.end() && guild->second != 0) ||
      (company_generation == coordinator.company_generations.end() ? 0 :
       company_generation->second) != membership.company_generation ||
      (guild_generation == coordinator.guild_generations.end() ? 0 :
       guild_generation->second) != membership.guild_generation) {
    return std::nullopt;
  }
  const auto owner = detail::find_owned_character(*database_, account_id, character_id, world_id);
  if (owner == database_->characters.end()) {
    return std::nullopt;
  }
  const auto existing = database_->world_progress.find(character_id);
  const WorldProgress empty_progress;
  const auto &current = existing == database_->world_progress.end() ? empty_progress : existing->second;
  if (current.revision != before.revision || current.dead != before.dead) {
    return std::nullopt;
  }
  auto prepared = detail::prepare_memory_loot_trade(*database_, *owner, after, trade);
  if (!prepared) {
    return std::nullopt;
  }
  auto granted = prepared->granted_items;
  auto [progress, inserted] = database_->world_progress.try_emplace(character_id);
  static_cast<void>(inserted);
  detail::publish_memory_loot_trade(*database_, *owner, progress->second, *prepared);
  return granted;
}

std::optional<QuestServiceMembership> MemoryCharacterStateRepository::read_quest_service_membership(
    std::int64_t account_id, std::int64_t character_id, int world_id) {
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  if (detail::find_owned_character(*database_, account_id, character_id, world_id) ==
      database_->characters.end()) {
    return std::nullopt;
  }
  const auto &coordinator = *database_->quest_services;
  QuestServiceMembership result;
  if (const auto found = coordinator.company_memberships.find(character_id);
      found != coordinator.company_memberships.end()) {
    result.company_type = found->second;
  }
  if (const auto found = coordinator.guild_memberships.find(character_id);
      found != coordinator.guild_memberships.end()) {
    result.guild_id = found->second;
  }
  if (const auto found = coordinator.company_generations.find(character_id);
      found != coordinator.company_generations.end()) {
    result.company_generation = found->second;
  }
  if (const auto found = coordinator.guild_generations.find(character_id);
      found != coordinator.guild_generations.end()) {
    result.guild_generation = found->second;
  }
  return result;
}

std::vector<QuestServiceEffectEvent> MemoryCharacterStateRepository::pending_quest_service_effects(
    int world_id, const std::optional<std::int64_t> &character_id, std::size_t limit,
    std::uint64_t after_sequence) {
  if (world_id <= 0 || (character_id && *character_id <= 0) || limit == 0 ||
      limit > kQuestServiceEffectPageSize ||
      after_sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument("Invalid quest service effect page");
  }
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  require_memory_reward_owner(*database_, monster_reward_owner_epoch_.load());
  std::vector<QuestServiceEffectEvent> result;
  result.reserve(limit);
  if (character_id) {
    const auto &coordinator = *database_->quest_services;
    const auto company = coordinator.pending_company.lower_bound({*character_id, 0});
    const auto guild = coordinator.pending_guild_by_character.lower_bound({*character_id, 0});
    const auto company_sequence = company != coordinator.pending_company.end() &&
                                          company->first == *character_id
                                      ? company->second : std::numeric_limits<std::uint64_t>::max();
    const auto guild_sequence = guild != coordinator.pending_guild_by_character.end() &&
                                        guild->first == *character_id
                                    ? guild->second : std::numeric_limits<std::uint64_t>::max();
    const auto sequence = std::min(company_sequence, guild_sequence);
    if (sequence == std::numeric_limits<std::uint64_t>::max() || sequence <= after_sequence) {
      return result;
    }
    const auto pending = coordinator.pending_order.find({world_id, sequence});
    if (pending == coordinator.pending_order.end()) {
      return result;
    }
    const auto event = coordinator.events.find(pending->second);
    if (event == coordinator.events.end() || event->second.character_id != *character_id ||
        event->second.enqueue_sequence != sequence) {
      throw std::logic_error("Quest service pending index is inconsistent");
    }
    result.push_back(event->second);
    return result;
  }
  const auto &coordinator = *database_->quest_services;
  for (auto pending = coordinator.pending_order.upper_bound({world_id, after_sequence});
       pending != database_->quest_services->pending_order.end() && pending->first.first == world_id;
       ++pending) {
    const auto sequence = pending->first.second;
    const auto &key = pending->second;
    const auto found = database_->quest_services->events.find(key);
    if (found == database_->quest_services->events.end()) {
      throw std::logic_error("Quest service pending index has no event");
    }
    const auto &event = found->second;
    if (event.enqueue_sequence != sequence || (event.company_applied && event.guild_applied)) {
      throw std::logic_error("Quest service pending index is inconsistent");
    }
    const auto company = coordinator.pending_company.lower_bound({event.character_id, 0});
    const auto guild = coordinator.pending_guild_by_character.lower_bound({event.character_id, 0});
    const auto company_sequence = company != coordinator.pending_company.end() &&
                                          company->first == event.character_id
                                      ? company->second : std::numeric_limits<std::uint64_t>::max();
    const auto guild_sequence = guild != coordinator.pending_guild_by_character.end() &&
                                        guild->first == event.character_id
                                    ? guild->second : std::numeric_limits<std::uint64_t>::max();
    if (event.world_id != world_id || sequence != std::min(company_sequence, guild_sequence)) {
      continue;
    }
    result.push_back(event);
    if (result.size() == limit) {
      break;
    }
  }
  return result;
}

QuestMutationResult MemoryCharacterStateRepository::commit_quest_mutation(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &effect_id, const WorldProgress &before,
    const std::optional<WorldProgress> &after, const ItemTrade &trade,
    const QuestServiceEffects &effects) {
  validate_quest_effect_request(effect_id, before, after, trade, effects);
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  return commit_memory_quest_mutation(*database_, monster_reward_owner_epoch_.load(), account_id,
                                      character_id, world_id, effect_id, before, after, trade,
                                      effects, false);
}

QuestMutationResult MemoryCharacterStateRepository::commit_combat_quest_event(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &effect_id, const std::string &source_fingerprint,
    const WorldProgress &before, const std::optional<WorldProgress> &after,
    const ItemTrade &trade, const QuestServiceEffects &effects) {
  if (account_id <= 0 || character_id <= 0 || world_id <= 0) {
    throw std::invalid_argument("Invalid combat quest event owner");
  }
  validate_quest_effect_request(effect_id, before, after, trade, effects);
  static_cast<void>(combat_quest_event_fingerprint(source_fingerprint, before, after, trade,
                                                   effects));
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  return commit_memory_quest_mutation(*database_, monster_reward_owner_epoch_.load(), account_id,
                                      character_id, world_id, effect_id, before, after, trade,
                                      effects, false, source_fingerprint);
}

MonsterRewardCommitResult MemoryCharacterStateRepository::commit_monster_reward_with_effects(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &receipt_id, const WorldProgress &before,
    const std::optional<WorldProgress> &after, const ItemTrade &trade,
    const QuestServiceEffects &effects) {
  validate_quest_effect_request(receipt_id, before, after, trade, effects);
  std::lock_guard coordinator_lock(database_->quest_services->mutex);
  std::lock_guard database_lock(database_->mutex);
  auto result = commit_memory_quest_mutation(*database_, monster_reward_owner_epoch_.load(),
                                              account_id, character_id, world_id, receipt_id,
                                              before, after, trade, effects, true);
  switch (result.status) {
  case QuestMutationStatus::committed:
    return {MonsterRewardCommitStatus::committed, std::move(result.granted_items)};
  case QuestMutationStatus::already_processed:
    return {MonsterRewardCommitStatus::already_processed, std::move(result.granted_items)};
  case QuestMutationStatus::conflict:
    return {};
  }
  throw std::logic_error("Unknown quest mutation status");
}

bool PostgresCharacterStateRepository::enqueue_monster_rewards(
    const std::vector<MonsterRewardReceipt> &receipts) {
  return enqueue_monster_rewards_with_script(receipts, std::nullopt);
}

bool PostgresCharacterStateRepository::enqueue_monster_rewards_with_script(
    const std::vector<MonsterRewardReceipt> &receipts,
    const std::optional<BossScriptIntent> &intent) {
  validate_monster_reward_with_script(receipts, intent);
  auto ordered = receipts;
  std::ranges::sort(ordered, [](const auto &left, const auto &right) {
    return left.character_id < right.character_id;
  });
  auto batch = nlohmann::json::array();
  for (const auto &receipt : ordered) {
    batch.push_back({{"account_id", receipt.account_id},
                     {"character_id", receipt.character_id},
                     {"world_id", receipt.world_id},
                     {"credited_unit_id", receipt.credited_unit_id},
                     {"npc_cms_id", receipt.npc_cms_id},
                     {"map_id", receipt.position.map_id},
                     {"x_mm", receipt.position.x_mm},
                     {"y_mm", receipt.position.y_mm},
                     {"z_mm", receipt.position.z_mm},
                     {"yaw", receipt.position.yaw},
                     {"award_experience", receipt.award_experience},
                     {"quest_attempts", monster_reward_attempts_json(receipt.quest_attempts)},
                     {"allow_monster_offered_quests", receipt.allow_monster_offered_quests}});
  }
  const auto batch_json = batch.dump();
  const auto script_json = intent ? boss_script_json(*intent).dump() : std::string{};
  const auto &event_id = intent ? intent->event_id : ordered.front().receipt_id;
  const int world_id = intent ? intent->world_id : ordered.front().world_id;

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto inserted_event = transaction.exec(
      "INSERT INTO emulator_monster_reward_events "
      "(receipt_id,world_id,recipients,boss_script,boss_script_applied,boss_script_published) "
      "VALUES ($1,$2,$3::jsonb,CASE WHEN $4::text='' THEN NULL ELSE $4::jsonb END,"
      "$4::text='',$4::text='') "
      "ON CONFLICT (receipt_id) DO NOTHING RETURNING receipt_id",
      pqxx::params{event_id, world_id, batch_json, script_json});
  const auto event = transaction.exec(
      "SELECT recipients=$2::jsonb AND world_id=$3 AND "
      "boss_script IS NOT DISTINCT FROM CASE WHEN $4::text='' THEN NULL ELSE $4::jsonb END "
      "FROM emulator_monster_reward_events "
      "WHERE receipt_id=$1 FOR UPDATE",
      pqxx::params{event_id, batch_json, world_id, script_json});
  if (event.empty() || !event.front()[0].as<bool>()) {
    return false;
  }
  const bool new_event = !inserted_event.empty();
  for (const auto &receipt : ordered) {
    const auto owner = transaction.exec(
        "SELECT id FROM emulator_characters WHERE account_id=$1 AND id=$2 AND world_id=$3 "
        "FOR UPDATE",
        pqxx::params{receipt.account_id, receipt.character_id, receipt.world_id});
    if (owner.empty()) {
      const auto character = transaction.exec(
          "SELECT id FROM emulator_characters WHERE id=$1", pqxx::params{receipt.character_id});
      if (new_event || !character.empty()) {
        return false;
      }
      continue;
    }
    if (new_event) {
      const auto count = transaction.exec(
          "SELECT count(*) FROM (SELECT 1 FROM emulator_monster_reward_receipts "
          "WHERE character_id=$1 AND NOT processed LIMIT $2) AS pending",
          pqxx::params{receipt.character_id, kMaximumPendingMonsterRewardsPerCharacter});
      if (count.front()[0].as<std::size_t>() >= kMaximumPendingMonsterRewardsPerCharacter) {
        return false;
      }
    }
  }
  for (const auto &receipt : ordered) {
    const auto &id = receipt.receipt_id;
    const auto owner = transaction.exec(
        "SELECT id FROM emulator_characters WHERE account_id=$1 AND id=$2 AND world_id=$3",
        pqxx::params{receipt.account_id, receipt.character_id, receipt.world_id});
    if (owner.empty()) {
      continue;
    }
    if (new_event) {
      transaction.exec(
          "INSERT INTO emulator_monster_reward_receipts(character_id,receipt_id,account_id,"
          "world_id,credited_unit_id,npc_cms_id,map_id,pos_x_mm,pos_y_mm,pos_z_mm,yaw,"
          "award_experience,quest_attempts,allow_monster_offered_quests) "
          "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13::jsonb,$14)",
          pqxx::params{receipt.character_id, id, receipt.account_id, receipt.world_id,
                       static_cast<std::int64_t>(receipt.credited_unit_id), receipt.npc_cms_id,
                       receipt.position.map_id, receipt.position.x_mm,
                       receipt.position.y_mm, receipt.position.z_mm, receipt.position.yaw,
                       receipt.award_experience,
                       monster_reward_attempts_json(receipt.quest_attempts).dump(),
                       receipt.allow_monster_offered_quests});
    }
    const auto stored = transaction.exec(
        "SELECT account_id,world_id,credited_unit_id,npc_cms_id,map_id,pos_x_mm,pos_y_mm,pos_z_mm,yaw,"
        "award_experience,quest_attempts,allow_monster_offered_quests "
        "FROM emulator_monster_reward_receipts "
        "WHERE character_id=$1 AND receipt_id=$2 FOR UPDATE",
        pqxx::params{receipt.character_id, id});
    if (stored.empty()) {
      return false;
    }
    const auto &row = stored.front();
    if (row[0].as<std::int64_t>() != receipt.account_id || row[1].as<int>() != receipt.world_id ||
        row[2].as<std::uint64_t>() != receipt.credited_unit_id ||
        row[3].as<int>() != receipt.npc_cms_id || row[4].as<int>() != receipt.position.map_id ||
        row[5].as<int>() != receipt.position.x_mm || row[6].as<int>() != receipt.position.y_mm ||
        row[7].as<int>() != receipt.position.z_mm || row[8].as<float>() != receipt.position.yaw ||
        row[9].as<bool>() != receipt.award_experience ||
        monster_reward_attempts_from_json(nlohmann::json::parse(row[10].as<std::string>())) !=
            receipt.quest_attempts ||
        row[11].as<bool>() != receipt.allow_monster_offered_quests) {
      return false;
    }
  }
  if (new_event) {
    // Reserve only after all recipient work. A blocked character cannot hold this world's
    // capacity counter while waiting for its own row, and unrelated worlds use other rows.
    const auto reserved = transaction.exec(
        "INSERT INTO emulator_monster_reward_limits(world_id,event_count) VALUES ($1,1) "
        "ON CONFLICT (world_id) DO UPDATE SET event_count="
        "emulator_monster_reward_limits.event_count+1 "
        "WHERE emulator_monster_reward_limits.event_count < $2 RETURNING event_count",
        pqxx::params{world_id, kMaximumStoredMonsterRewardEvents});
    if (reserved.empty()) {
      return false;
    }
  }
  transaction.commit();
  return true;
}

std::vector<BossScriptIntent> PostgresCharacterStateRepository::pending_boss_scripts(
    int world_id, const std::optional<std::string> &after, std::size_t limit) {
  if (world_id <= 0 || limit == 0 || limit > kMonsterRewardRecoveryPageSize ||
      (after && !valid_monster_reward_id(*after))) {
    throw std::invalid_argument("Invalid boss script recovery page");
  }
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto rows = transaction.exec(
      "SELECT receipt_id,boss_script FROM emulator_monster_reward_events "
      "WHERE world_id=$1 AND ready AND NOT boss_script_published AND receipt_id>$2 "
      "ORDER BY receipt_id LIMIT $3", pqxx::params{world_id, after.value_or(""), limit});
  std::vector<BossScriptIntent> result;
  result.reserve(rows.size());
  for (const auto &row : rows) {
    if (row[1].is_null()) {
      throw std::runtime_error("Pending monster event lost its boss script");
    }
    auto intent = boss_script_from_json(nlohmann::json::parse(row[1].as<std::string>()));
    if (intent.world_id != world_id || intent.event_id != row[0].as<std::string>()) {
      throw std::runtime_error("Pending boss script identity does not match its event");
    }
    result.push_back(std::move(intent));
  }
  transaction.commit();
  return result;
}

BossScriptSettlement PostgresCharacterStateRepository::settle_shared_boss_script(
    const BossScriptIntent &intent) {
  const auto targets = boss_script_targets(intent);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto events = transaction.exec(
      "SELECT ready,boss_script_applied,boss_script=$2::jsonb "
      "FROM emulator_monster_reward_events WHERE receipt_id=$1 AND world_id=$3 FOR UPDATE",
      pqxx::params{intent.event_id, boss_script_json(intent).dump(), intent.world_id});
  if (events.empty() || !events.front()[0].as<bool>() || events.front()[2].is_null() ||
      !events.front()[2].as<bool>()) {
    return {};
  }
  const bool already_applied = events.front()[1].as<bool>();
  std::size_t inserted_count = 0;
  std::vector<SharedGatheringSourceRecord> result;
  std::string event_id = intent.event_id;
  result.reserve(targets.size());
  for (const auto &target : targets) {
    if (!already_applied) {
      const auto inserted = transaction.exec(
          "INSERT INTO emulator_gathering_sources "
          "(world_id,map_id,source_kind,sector_id,local_index,generation,active,phase) "
          "VALUES ($1,$2,$3,$4,$5,0,true,$6) ON CONFLICT DO NOTHING RETURNING world_id",
          pqxx::params{intent.world_id, target.key.map_id, target.key.kind,
                       target.key.sector_id, target.key.local_index, target.phase});
      if (!inserted.empty()) {
        ++inserted_count;
      }
    }
    const auto rows = transaction.exec(
        "SELECT generation,active,phase,pending_receipt_id,next_transition_epoch_ticks,"
        "timed_successor_phase,next_transition_delay_ticks FROM emulator_gathering_sources "
        "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5 "
        "FOR UPDATE", pqxx::params{intent.world_id, target.key.map_id, target.key.kind,
                                    target.key.sector_id, target.key.local_index});
    if (rows.empty()) {
      throw std::runtime_error("Settled boss script lost a source");
    }
    auto snapshot = detail::decode_shared_gathering_source(rows.front());
    if (snapshot.pending_receipt_id) {
      return {};
    }
    if (snapshot.generation >= std::numeric_limits<std::int64_t>::max() - 1) {
      throw std::overflow_error("Boss script source generation exhausted");
    }
    ++snapshot.generation;
    result.push_back({target.key, std::move(snapshot)});
  }
  if (inserted_count != 0 &&
      !detail::reserve_shared_source_capacity(transaction, intent.world_id, inserted_count)) {
    return {};
  }
  for (const auto &target : targets) {
    transaction.exec(
        "UPDATE emulator_gathering_sources SET generation=generation+1 "
        "WHERE world_id=$1 AND map_id=$2 AND source_kind=$3 AND sector_id=$4 AND local_index=$5",
        pqxx::params{intent.world_id, target.key.map_id, target.key.kind,
                     target.key.sector_id, target.key.local_index});
  }
  transaction.exec("UPDATE emulator_monster_reward_events SET boss_script_applied=true "
                   "WHERE receipt_id=$1", pqxx::params{intent.event_id});
  transaction.commit();
  return {true, std::move(result), std::move(event_id)};
}

bool PostgresCharacterStateRepository::mark_boss_script_published(const BossScriptIntent &intent) {
  validate_boss_script_intent(intent);
  if (intent.copy_scope != "shared") {
    throw std::invalid_argument("Private boss scripts require copy-local acknowledgement");
  }
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto rows = transaction.exec(
      "UPDATE emulator_monster_reward_events SET boss_script_published=true "
      "WHERE receipt_id=$1 AND world_id=$2 AND ready AND boss_script_applied "
      "AND boss_script=$3::jsonb RETURNING receipt_id",
      pqxx::params{intent.event_id, intent.world_id, boss_script_json(intent).dump()});
  transaction.commit();
  return !rows.empty();
}

bool PostgresCharacterStateRepository::mark_private_boss_script_applied(
    const BossScriptIntent &intent) {
  validate_boss_script_intent(intent);
  if (intent.copy_scope == "shared") {
    throw std::invalid_argument("Shared boss scripts require durable source settlement");
  }
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto rows = transaction.exec(
      "UPDATE emulator_monster_reward_events SET boss_script_applied=true,boss_script_published=true "
      "WHERE receipt_id=$1 AND world_id=$2 AND ready AND boss_script=$3::jsonb "
      "RETURNING receipt_id", pqxx::params{intent.event_id, intent.world_id,
                                            boss_script_json(intent).dump()});
  transaction.commit();
  return !rows.empty();
}

bool PostgresCharacterStateRepository::retire_private_boss_script(const BossScriptIntent &intent) {
  return mark_private_boss_script_applied(intent);
}

std::int64_t PostgresCharacterStateRepository::acquire_monster_reward_owner_epoch() {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  const auto rows = transaction.exec(
      "UPDATE emulator_monster_reward_server_owner SET epoch=epoch+1 "
      "WHERE singleton AND epoch < $1 RETURNING epoch",
      pqxx::params{std::numeric_limits<std::int64_t>::max()});
  if (rows.size() != 1) {
    throw std::runtime_error("Monster reward owner epoch is missing or exhausted");
  }
  const auto epoch = rows.front()[0].as<std::int64_t>();
  transaction.commit();
  monster_reward_owner_epoch_.store(epoch);
  return epoch;
}

void PostgresCharacterStateRepository::adopt_monster_reward_owner_epoch(std::int64_t epoch) {
  if (epoch <= 0) {
    throw std::invalid_argument("Invalid monster reward owner epoch");
  }
  monster_reward_owner_epoch_.store(epoch);
}

bool PostgresCharacterStateRepository::mark_monster_reward_ready(const std::string &receipt_id) {
  if (!valid_monster_reward_id(receipt_id)) {
    throw std::invalid_argument("Invalid monster reward receipt ID");
  }
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto updated = transaction.exec(
      "UPDATE emulator_monster_reward_events SET ready=true WHERE receipt_id=$1 "
      "RETURNING receipt_id", pqxx::params{receipt_id});
  if (updated.empty()) {
    return false;
  }
  transaction.commit();
  return true;
}

std::size_t PostgresCharacterStateRepository::recover_staged_monster_rewards(std::size_t limit) {
  if (limit == 0 || limit > kMonsterRewardRecoveryPageSize) {
    throw std::invalid_argument("Invalid monster reward recovery page size");
  }
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto recovered = transaction.exec(
      "WITH selected AS (SELECT receipt_id FROM emulator_monster_reward_events "
      "WHERE NOT ready ORDER BY receipt_id LIMIT $1 FOR UPDATE) "
      "UPDATE emulator_monster_reward_events AS events SET ready=true FROM selected "
      "WHERE events.receipt_id=selected.receipt_id RETURNING events.receipt_id",
      pqxx::params{limit});
  const auto count = recovered.size();
  transaction.commit();
  return count;
}

bool PostgresCharacterStateRepository::has_staged_monster_rewards() {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto rows = transaction.exec(
      "SELECT EXISTS (SELECT 1 FROM emulator_monster_reward_events WHERE NOT ready)");
  return rows.front()[0].as<bool>();
}

std::vector<MonsterRewardReceipt> PostgresCharacterStateRepository::pending_monster_rewards(
    std::int64_t account_id, std::int64_t character_id, int world_id) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
  const auto owner = transaction.exec(
      "SELECT id FROM emulator_characters WHERE account_id=$1 AND id=$2 AND world_id=$3",
      pqxx::params{account_id, character_id, world_id});
  if (owner.empty()) {
    throw std::runtime_error("Monster reward character is not owned by this account");
  }
  const auto rows = transaction.exec(
      "SELECT receipt.receipt_id,receipt.credited_unit_id,receipt.npc_cms_id,receipt.map_id,"
      "receipt.pos_x_mm,receipt.pos_y_mm,receipt.pos_z_mm,receipt.yaw,"
      "receipt.award_experience,receipt.quest_attempts,receipt.allow_monster_offered_quests "
      "FROM emulator_monster_reward_receipts AS receipt "
      "JOIN emulator_monster_reward_events AS event ON event.receipt_id=receipt.receipt_id "
      "WHERE receipt.character_id=$1 AND receipt.account_id=$2 "
      "AND receipt.world_id=$3 AND NOT receipt.processed AND event.ready "
      "AND event.boss_script_published "
      "AND NOT EXISTS (SELECT 1 FROM emulator_monster_reward_receipts AS earlier "
      "JOIN emulator_monster_reward_events AS earlier_event "
      "ON earlier_event.receipt_id=earlier.receipt_id WHERE earlier.character_id=$1 "
      "AND NOT earlier.processed AND (NOT earlier_event.ready OR NOT earlier_event.boss_script_published) "
      "AND earlier.enqueue_sequence < receipt.enqueue_sequence) "
      "ORDER BY receipt.enqueue_sequence LIMIT $4",
      pqxx::params{character_id, account_id, world_id, kMonsterRewardPageSize});

  std::vector<MonsterRewardReceipt> pending;
  pending.reserve(rows.size());
  for (const auto &row : rows) {
    MonsterRewardReceipt receipt{account_id, character_id, world_id,
                                 row[0].as<std::string>(), row[1].as<std::uint64_t>(),
                                 row[2].as<int>(),
                                 {row[3].as<int>(), row[4].as<int>(), row[5].as<int>(),
                                  row[6].as<int>(), row[7].as<float>()},
                                 row[8].as<bool>(),
                                 monster_reward_attempts_from_json(
                                     nlohmann::json::parse(row[9].as<std::string>())),
                                 row[10].as<bool>()};
    validate_monster_reward(receipt);
    pending.push_back(receipt);
  }
  return pending;
}

MonsterRewardCommitResult PostgresCharacterStateRepository::commit_monster_reward(
    std::int64_t account_id, std::int64_t character_id, int world_id,
    const std::string &receipt_id,
    const WorldProgress &before, const std::optional<WorldProgress> &after, const ItemTrade &trade) {
  validate_item_trade(trade);
  if (!valid_monster_reward_id(receipt_id)) {
    throw std::invalid_argument("Invalid monster reward receipt ID");
  }
  if (after) {
    validate_transition(before, *after);
  } else if (!trade.granted_items.empty() || !trade.removed_item_ids.empty() ||
             !trade.durability_updates.empty()) {
    throw std::invalid_argument("Item changes need a monster reward progress transition");
  }

  try {
    pqxx::connection connection(connection_);
    pqxx::work transaction(connection);
    detail::require_reward_owner_epoch(transaction, monster_reward_owner_epoch_.load());
    // Match enqueue's event-then-character-then-receipt lock order.
    const auto event = transaction.exec(
        "SELECT ready,boss_script_published FROM emulator_monster_reward_events WHERE receipt_id=$1 FOR SHARE",
        pqxx::params{receipt_id});
    if (event.empty() || !event.front()[0].as<bool>() || !event.front()[1].as<bool>()) {
      return {};
    }
    const auto owner = transaction.exec(
        "SELECT gameplay_revision,player_dead FROM emulator_characters "
        "WHERE account_id=$1 AND id=$2 AND world_id=$3 FOR UPDATE",
        pqxx::params{account_id, character_id, world_id});
    if (owner.empty()) {
      return {};
    }
    const auto &id = receipt_id;
    const auto rows = transaction.exec(
        "SELECT processed,enqueue_sequence FROM emulator_monster_reward_receipts "
        "WHERE character_id=$1 AND receipt_id=$2 AND account_id=$3 AND world_id=$4 FOR UPDATE",
        pqxx::params{character_id, id, account_id, world_id});
    if (rows.empty()) {
      return {};
    }
    if (rows.front()[0].as<bool>()) {
      return {MonsterRewardCommitStatus::already_processed, {}};
    }
    const auto earlier = transaction.exec(
        "SELECT 1 FROM emulator_monster_reward_receipts AS receipt "
        "JOIN emulator_monster_reward_events AS event ON event.receipt_id=receipt.receipt_id "
        "WHERE receipt.character_id=$1 AND NOT receipt.processed "
        "AND (NOT event.ready OR NOT event.boss_script_published) "
        "AND receipt.enqueue_sequence < $2 LIMIT 1",
        pqxx::params{character_id, rows.front()[1].as<std::int64_t>()});
    if (!earlier.empty()) {
      return {};
    }
    if (owner.front()[0].as<std::int64_t>() != before.revision ||
        owner.front()[1].as<bool>() != before.dead) {
      return {};
    }
    std::vector<CharacterItem> items;
    if (after) {
      auto committed = apply_postgres_trade(transaction, account_id, character_id, world_id,
                                            before, *after, trade);
      if (!committed) {
        return {};
      }
      items = std::move(*committed);
    }
    transaction.exec(
        "UPDATE emulator_monster_reward_receipts SET processed=true "
        "WHERE character_id=$1 AND receipt_id=$2",
        pqxx::params{character_id, id});
    transaction.commit();
    return {MonsterRewardCommitStatus::committed, std::move(items)};
  } catch (const pqxx::unique_violation &) {
    return {};
  }
}

} // namespace chronicles
