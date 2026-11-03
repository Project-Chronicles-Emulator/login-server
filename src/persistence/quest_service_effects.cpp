#include "persistence/quest_service_effects.hpp"
#include "persistence/quest_service_effect_codec.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace chronicles {
namespace {

using Json = nlohmann::json;

Json item_json(const CharacterItem &item) {
  return {{"id", item.id},
          {"cms_id", item.cms_id},
          {"inventory_slot", item.inventory_slot},
          {"equip_slot", item.equip_slot ? Json(*item.equip_slot) : Json(nullptr)},
          {"quantity", item.quantity},
          {"durability", item.durability},
          {"grade", item.grade},
          {"polish_step", item.polish_step},
          {"created_epoch_ticks", item.created_epoch_ticks}};
}

Json position_json(const WorldPosition &position) {
  return {{"map_id", position.map_id}, {"x_mm", position.x_mm}, {"y_mm", position.y_mm},
          {"z_mm", position.z_mm}, {"yaw", position.yaw}};
}

Json progress_json(const WorldProgress &progress) {
  Json quests = Json::array();
  for (const auto &[id, quest] : progress.quests) {
    quests.push_back({{"id", id}, {"complete", quest.complete},
                      {"success_count", quest.success_count}, {"objectives", quest.objectives},
                      {"credited_kills", quest.credited_kills}, {"attempt", quest.attempt},
                      {"abandoned", quest.abandoned},
                      {"started_epoch_ms", quest.started_epoch_ms}});
  }
  Json utility = Json::array();
  for (const auto &slot : progress.utility_slots) {
    utility.push_back({{"item_cms_id", slot.item_cms_id}, {"count", slot.count}});
  }
  Json purchases = Json::array();
  for (const auto &[key, value] : progress.shop_purchases) {
    purchases.push_back({{"shop_id", key.shop_id}, {"item_cms_id", key.item_cms_id},
                         {"purchased_count", value.purchased_count},
                         {"reset_epoch_ticks", value.reset_epoch_ticks}});
  }
  Json housing = Json::array();
  for (const auto &[id, value] : progress.housing_items) {
    housing.push_back({{"id", id}, {"quantity", value.quantity},
                       {"installed_count", value.installed_count}});
  }
  Json vouchers = Json::array();
  for (const auto &[id, value] : progress.trade_vouchers) {
    vouchers.push_back({{"id", id}, {"good_cms_id", value.good_cms_id},
                        {"created_epoch_ticks", value.created_epoch_ticks},
                        {"expiration_epoch_ticks", value.expiration_epoch_ticks},
                        {"redeemed", value.redeemed}});
  }
  Json mates = Json::array();
  for (const auto &[id, value] : progress.mates) {
    mates.push_back({{"id", id}, {"state", value.state},
                     {"housing_lot_number", value.housing_lot_number},
                     {"intimacy", value.intimacy}});
  }
  Json goods = Json::array();
  for (const auto &good : progress.trade_run.goods) {
    goods.push_back({{"good_cms_id", good.good_cms_id}, {"origin_post_id", good.origin_post_id},
                     {"production_epoch_ticks", good.production_epoch_ticks}});
  }
  const auto &run = progress.trade_run;
  Json trade_run{{"phase", static_cast<int>(run.phase)}, {"start_post_id", run.start_post_id},
                 {"parked_post_id", run.parked_post_id}, {"vehicle_cms_id", run.vehicle_cms_id},
                 {"started_epoch_ticks", run.started_epoch_ticks}, {"goods", goods},
                 {"wagon_position", run.wagon_position ? position_json(*run.wagon_position)
                                                        : Json(nullptr)},
                 {"wagon_health", run.wagon_health},
                 {"last_wagon_spawn_epoch_ticks", run.last_wagon_spawn_epoch_ticks}};
  Json achievements = Json::array();
  for (const auto &[id, value] : progress.achievements) {
    achievements.push_back({{"id", id}, {"state", value.state},
                            {"completion_tick", value.completion_tick}});
  }
  Json boards = Json::array();
  for (const auto &[id, value] : progress.job_boards) {
    boards.push_back({{"id", id}, {"day", value.day}, {"refreshes", value.refreshes}});
  }
  return {{"revision", progress.revision}, {"dead", progress.dead}, {"gold", progress.gold},
          {"hersander_token", progress.hersander_token}, {"experience", progress.experience},
          {"level", progress.level}, {"level_up_points", progress.level_up_points},
          {"weapon_points", progress.weapon_points},
          {"nui_blessing_level", progress.nui_blessing_level},
          {"dedicated_nui_tears", progress.dedicated_nui_tears},
          {"build", {{"active_weapon_slot", progress.build.active_weapon_slot},
                      {"stat_bonuses", progress.build.stat_bonuses},
                      {"perks", progress.build.perks}}},
          {"stackables", progress.stackables}, {"adventure_objects", progress.adventure_objects},
          {"quests", quests}, {"doodad_phases", progress.doodad_phases},
          {"utility_slots", utility}, {"shop_purchases", purchases}, {"housing_items", housing},
          {"trade_vouchers", vouchers}, {"registered_summons", progress.registered_summons},
          {"mates", mates}, {"trade_run", trade_run}, {"loot_first_kills", progress.loot_first_kills},
          {"owned_emotions", progress.owned_emotions}, {"learned_crafts", progress.learned_crafts},
          {"achievements", achievements}, {"job_boards", boards},
          {"play_records", progress.play_records}};
}

std::string fingerprint_document(const Json &document) {
  const auto encoded = document.dump();
  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned int length{};
  if (EVP_Digest(encoded.data(), encoded.size(), digest, &length, EVP_sha256(), nullptr) != 1 ||
      length != 32) {
    throw std::runtime_error("Could not fingerprint the mutation");
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(length * 2);
  for (unsigned int index = 0; index < length; ++index) {
    result.push_back(hex[digest[index] >> 4]);
    result.push_back(hex[digest[index] & 15]);
  }
  return result;
}

} // namespace

nlohmann::json canonical_quest_mutation(const WorldProgress &before,
                                         const std::optional<WorldProgress> &after,
                                         const ItemTrade &trade,
                                         const QuestServiceEffects &effects) {
  if (effects.activity_experience > 0 &&
      (!after || before.experience < 0 || after->experience <= before.experience ||
       after->experience - before.experience != effects.activity_experience)) {
    throw std::invalid_argument("Activity EXP must match the committed progress award");
  }
  Json granted = Json::array();
  for (const auto &item : trade.granted_items) {
    granted.push_back(item_json(item));
  }
  Json durability = Json::array();
  for (const auto &item : trade.durability_updates) {
    durability.push_back({{"item_id", item.item_id}, {"expected_durability", item.expected_durability},
                          {"durability", item.durability}});
  }
  return {{"version", 1}, {"before", progress_json(before)},
          {"after", after ? progress_json(*after) : Json(nullptr)},
          {"trade", {{"granted_items", granted}, {"removed_item_ids", trade.removed_item_ids},
                     {"durability_updates", durability}}},
          {"effects", encode_quest_service_effects(effects)}};
}

std::string quest_mutation_fingerprint(const WorldProgress &before,
                                        const std::optional<WorldProgress> &after,
                                        const ItemTrade &trade,
                                        const QuestServiceEffects &effects) {
  return fingerprint_document(canonical_quest_mutation(before, after, trade, effects));
}

std::string combat_quest_source_fingerprint(const nlohmann::json &source) {
  return fingerprint_document({{"kind", "combat-quest-source-v1"}, {"source", source}});
}

std::string combat_quest_event_fingerprint(const std::string &source_fingerprint,
                                          const WorldProgress &before,
                                          const std::optional<WorldProgress> &after,
                                          const ItemTrade &trade,
                                          const QuestServiceEffects &effects) {
  const bool valid = source_fingerprint.size() == 64 &&
      std::ranges::all_of(source_fingerprint, [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
      });
  if (!valid) {
    throw std::invalid_argument("Invalid combat quest event source fingerprint");
  }

  return fingerprint_document({{"kind", "combat-quest-event-v1"},
                               {"source_fingerprint", source_fingerprint},
                               {"mutation", canonical_quest_mutation(before, after, trade, effects)}});
}

std::string gathering_mutation_fingerprint(
    const std::string &source_fingerprint, const WorldProgress &before,
    const WorldProgress &after, const ItemTrade &trade, const QuestServiceEffects &effects,
    const std::optional<SharedGatheringSourceTransition> &source) {
  Json source_document = nullptr;
  if (source) {
    source_document = {{"map_id", source->map_id}, {"kind", source->kind},
                       {"sector_id", source->sector_id}, {"local_index", source->local_index},
                       {"expected_generation", source->expected_generation},
                       {"expected_active", source->expected_active},
                       {"expected_phase", source->expected_phase ? Json(*source->expected_phase) : Json(nullptr)},
                       {"resulting_active", source->resulting_active},
                       {"resulting_phase", source->resulting_phase ? Json(*source->resulting_phase) : Json(nullptr)},
                       {"next_transition_delay_ticks", source->next_transition_delay_ticks
                           ? Json(*source->next_transition_delay_ticks) : Json(nullptr)},
                       {"timed_successor_phase", source->timed_successor_phase
                           ? Json(*source->timed_successor_phase) : Json(nullptr)}};
    if (source->linked) {
      const auto &linked = *source->linked;
      source_document["linked"] = {
          {"map_id", linked.map_id}, {"kind", linked.kind},
          {"sector_id", linked.sector_id}, {"local_index", linked.local_index},
          {"expected_generation", linked.expected_generation},
          {"expected_active", linked.expected_active},
          {"expected_phase", linked.expected_phase ? Json(*linked.expected_phase) : Json(nullptr)}};
    }
  }
  return fingerprint_document({{"kind", "gathering-v1"},
                               {"source_fingerprint", source_fingerprint},
                               {"source", source_document},
                               {"mutation", canonical_quest_mutation(before, after, trade, effects)}});
}

void validate_shared_gathering_source(const SharedGatheringSourceTransition &source) {
  if (source.map_id <= 0 || (source.kind != 0 && source.kind != 1) ||
      source.expected_generation < 0 ||
      source.expected_generation >= std::numeric_limits<std::int64_t>::max() - 1 ||
      (source.expected_phase && *source.expected_phase < 0) ||
      (source.resulting_phase && *source.resulting_phase < 0) ||
      (source.next_transition_delay_ticks && *source.next_transition_delay_ticks <= 0) ||
      (source.timed_successor_phase && *source.timed_successor_phase < 0) ||
      (source.timed_successor_phase && !source.next_transition_delay_ticks)) {
    throw std::invalid_argument("Invalid shared gathering source transition");
  }
  if (source.linked) {
    constexpr int kCliffsMap = 30104;
    constexpr int kDoodadKind = 1;
    constexpr std::uint32_t kCannonSector = 171 | (290 << 14);
    constexpr std::uint32_t kBarrierSector = 171 | (289 << 14);
    const auto &linked = *source.linked;
    // This exact unprofiled barrier may carry a non-rendered unknown-baseline marker.
    const bool unknown_initial = !linked.expected_active && !linked.expected_phase;
    const bool already_open = linked.expected_generation > 0 && !linked.expected_active &&
                              linked.expected_phase == 0;
    if (source.map_id != kCliffsMap || source.kind != kDoodadKind ||
        source.sector_id != kCannonSector || source.local_index != 1 ||
        source.expected_phase != 1 || linked.map_id != kCliffsMap ||
        linked.kind != kDoodadKind || linked.sector_id != kBarrierSector ||
        linked.local_index != 2 || linked.expected_generation < 0 ||
        linked.expected_generation >= std::numeric_limits<std::int64_t>::max() - 1 ||
        (!unknown_initial && !already_open)) {
      throw std::invalid_argument("Unsupported linked gathering script transition");
    }
  }
}

} // namespace chronicles
