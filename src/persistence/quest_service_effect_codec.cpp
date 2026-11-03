#include "persistence/quest_service_effect_codec.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaximumUnlocks = 128;
constexpr std::size_t kMaximumGrantedRows = 64;
constexpr int kMaximumCompanyPositionLevel = 100;

std::int64_t bounded_integer(const Json &value, std::int64_t minimum, std::int64_t maximum) {
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number < static_cast<std::uint64_t>(minimum) ||
        number > static_cast<std::uint64_t>(maximum)) {
      throw std::invalid_argument("Quest effect integer is outside its supported range");
    }
    return static_cast<std::int64_t>(number);
  }
  if (!value.is_number_integer()) {
    throw std::invalid_argument("Quest effect value must be an integer");
  }
  const auto number = value.get<std::int64_t>();
  if (number < minimum || number > maximum) {
    throw std::invalid_argument("Quest effect integer is outside its supported range");
  }
  return number;
}

void validate_effects(const QuestServiceEffects &effects) {
  if (effects.company_unlocks.size() > kMaximumUnlocks || effects.company_contribution < 0 ||
      effects.guild_experience < 0 || effects.activity_experience < 0 ||
      effects.activity_company_rate < 0 || effects.activity_guild_rate < 0 ||
      (effects.activity_experience > 0 && effects.expected_company_type != 0 &&
       effects.activity_company_rate == 0) ||
      (effects.activity_experience > 0 && effects.expected_guild_id != 0 &&
       effects.activity_guild_rate == 0) || effects.expected_company_type < 0 ||
      effects.expected_company_type > 5 || effects.expected_guild_id < 0 ||
      effects.expected_company_generation < 0 || effects.expected_guild_generation < 0) {
    throw std::invalid_argument("Invalid quest service effects");
  }
  for (const auto &unlock : effects.company_unlocks) {
    if (unlock.company_type <= 0 || unlock.company_type > 5 || unlock.level <= 0 ||
        unlock.level > kMaximumCompanyPositionLevel) {
      throw std::invalid_argument("Invalid quest company unlock");
    }
  }
}

} // namespace

nlohmann::json encode_quest_service_effects(const QuestServiceEffects &effects) {
  validate_effects(effects);
  Json unlocks = Json::array();
  for (const auto &unlock : effects.company_unlocks) {
    unlocks.push_back({{"company_type", unlock.company_type}, {"level", unlock.level}});
  }
  return {{"company_unlocks", unlocks}, {"company_contribution", effects.company_contribution},
          {"guild_experience", effects.guild_experience},
          {"activity_experience", effects.activity_experience},
          {"activity_company_rate", effects.activity_company_rate},
          {"activity_guild_rate", effects.activity_guild_rate},
          {"expected_company_type", effects.expected_company_type},
          {"expected_guild_id", effects.expected_guild_id},
          {"expected_company_generation", effects.expected_company_generation},
          {"expected_guild_generation", effects.expected_guild_generation}};
}

QuestServiceEffects decode_quest_service_effects(const nlohmann::json &document) {
  if (!document.is_object() || document.size() != 10 ||
      !document.at("company_unlocks").is_array() ||
      document.at("company_unlocks").size() > kMaximumUnlocks) {
    throw std::invalid_argument("Invalid stored quest service effect payload");
  }
  QuestServiceEffects effects;
  for (const auto &entry : document.at("company_unlocks")) {
    if (!entry.is_object() || entry.size() != 2) {
      throw std::invalid_argument("Invalid stored quest company unlock");
    }
    effects.company_unlocks.push_back({
        static_cast<int>(bounded_integer(entry.at("company_type"), 1, 5)),
        static_cast<int>(bounded_integer(entry.at("level"), 1, kMaximumCompanyPositionLevel))});
  }
  constexpr auto max_value = std::numeric_limits<std::int64_t>::max();
  effects.company_contribution = bounded_integer(document.at("company_contribution"), 0, max_value);
  effects.guild_experience = bounded_integer(document.at("guild_experience"), 0, max_value);
  effects.activity_experience = bounded_integer(document.at("activity_experience"), 0, max_value);
  effects.activity_company_rate = bounded_integer(document.at("activity_company_rate"), 0, max_value);
  effects.activity_guild_rate = bounded_integer(document.at("activity_guild_rate"), 0, max_value);
  effects.expected_company_type =
      static_cast<int>(bounded_integer(document.at("expected_company_type"), 0, 5));
  effects.expected_guild_id = bounded_integer(document.at("expected_guild_id"), 0, max_value);
  effects.expected_company_generation =
      bounded_integer(document.at("expected_company_generation"), 0, max_value);
  effects.expected_guild_generation =
      bounded_integer(document.at("expected_guild_generation"), 0, max_value);
  validate_effects(effects);
  return effects;
}

nlohmann::json encode_quest_granted_items(const std::vector<CharacterItem> &items) {
  if (items.size() > kMaximumGrantedRows) {
    throw std::invalid_argument("Too many quest reward item rows");
  }
  Json result = Json::array();
  for (const auto &item : items) {
    if (item.id <= 0 || item.cms_id <= 0 || item.inventory_slot < 0 ||
        item.inventory_slot > kMaximumInventorySlot || item.equip_slot || item.quantity <= 0 ||
        item.durability < 0 || item.grade < 0 || item.grade > 255 || item.polish_step < 0 ||
        item.created_epoch_ticks < 0) {
      throw std::invalid_argument("Invalid quest reward item row");
    }
    result.push_back({{"id", item.id}, {"cms_id", item.cms_id},
                      {"inventory_slot", item.inventory_slot}, {"equip_slot", nullptr},
                      {"quantity", item.quantity}, {"durability", item.durability},
                      {"grade", item.grade}, {"polish_step", item.polish_step},
                      {"created_epoch_ticks", item.created_epoch_ticks}});
  }
  return result;
}

std::vector<CharacterItem> decode_quest_granted_items(const nlohmann::json &document) {
  if (!document.is_array() || document.size() > kMaximumGrantedRows) {
    throw std::invalid_argument("Invalid stored quest reward item rows");
  }
  std::vector<CharacterItem> result;
  result.reserve(document.size());
  for (const auto &row : document) {
    if (!row.is_object() || row.size() != 9 || !row.at("equip_slot").is_null()) {
      throw std::invalid_argument("Invalid stored quest reward item row");
    }
    CharacterItem item;
    constexpr auto max_value = std::numeric_limits<std::int64_t>::max();
    constexpr auto max_int = std::numeric_limits<int>::max();
    item.id = bounded_integer(row.at("id"), 1, max_value);
    item.cms_id = static_cast<int>(bounded_integer(row.at("cms_id"), 1, max_int));
    item.inventory_slot =
        static_cast<int>(bounded_integer(row.at("inventory_slot"), 0, kMaximumInventorySlot));
    item.quantity = static_cast<int>(bounded_integer(row.at("quantity"), 1, max_int));
    item.durability = static_cast<int>(bounded_integer(row.at("durability"), 0, max_int));
    item.grade = static_cast<int>(bounded_integer(row.at("grade"), 0, 255));
    item.polish_step = static_cast<int>(bounded_integer(row.at("polish_step"), 0, max_int));
    item.created_epoch_ticks = bounded_integer(row.at("created_epoch_ticks"), 0, max_value);
    result.push_back(item);
  }
  // Reuse the write-side bounds on decoded rows before returning them to a caller.
  static_cast<void>(encode_quest_granted_items(result));
  return result;
}

} // namespace chronicles
