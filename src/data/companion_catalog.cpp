#include "data/companion_catalog.hpp"

#include "common/catalog_fields.hpp"
#include "data/client_catalog.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace chronicles::companion {
namespace {

using Json = nlohmann::json;
namespace fields = catalog_fields;

constexpr std::string_view kMountAbility = "Mount";
constexpr std::string_view kTradeWagonType = "TradeWagon";
constexpr std::string_view kShipType = "Ship";
constexpr std::string_view kGoldCurrency = "Gold";
constexpr std::string_view kTokenCurrency = "HersanderToken";
constexpr std::string_view kRegisterSummonEffect = "registerSummonNpc";
constexpr std::string_view kRepairWagonTask = "RepairWagon";
constexpr std::string_view kConsumeItemCondition = "ConsumeItem";
constexpr int kBasisPointsPerWhole = 10000;

// Several catalog columns hold a JSON object body without its braces, for example
// "\"cmsId\":2001". An empty column is an empty object.
Json braced_object(const Json &row, const char *field) {
  const auto body = fields::text(row, field);
  if (body.empty()) {
    return Json::object();
  }

  auto parsed = Json::parse("{" + body + "}", nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    throw std::runtime_error(std::string("Invalid catalog object in field ") + field);
  }
  return parsed;
}

int positive_json_int(const Json &object, const char *field) {
  const auto found = object.find(field);
  if (found == object.end() || !found->is_number_integer()) {
    throw std::runtime_error(std::string("Missing catalog integer member ") + field);
  }

  const auto value = found->get<std::int64_t>();
  if (value <= 0 || value > std::numeric_limits<int>::max()) {
    throw std::runtime_error(std::string("Catalog integer member out of range: ") + field);
  }
  return static_cast<int>(value);
}

// Item.FK_skillEffectList is a JSON array of SkillEffect ids, for example "[2001]".
std::vector<int> skill_effect_list(const Json &row) {
  const auto text = fields::text(row, "FK_skillEffectList");
  if (text.empty()) {
    return {};
  }

  const auto parsed = Json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_array()) {
    throw std::runtime_error("Invalid Item.FK_skillEffectList");
  }

  std::vector<int> effect_ids;
  for (const auto &entry : parsed) {
    if (!entry.is_number_integer() || entry.get<std::int64_t>() <= 0 ||
        entry.get<std::int64_t>() > std::numeric_limits<int>::max()) {
      throw std::runtime_error("Invalid Item.FK_skillEffectList entry");
    }
    effect_ids.push_back(entry.get<int>());
  }
  return effect_ids;
}

// DoodadMateInteraction.FK_mateNpcs is a parenthesized id list, for example "(1,2)".
std::set<int> parenthesized_ids(const Json &row, const char *field) {
  auto text = fields::text(row, field);
  if (text.size() < 2 || text.front() != '(' || text.back() != ')') {
    throw std::runtime_error(std::string("Invalid catalog id list in field ") + field);
  }

  std::set<int> ids;
  std::string_view remaining(text.data() + 1, text.size() - 2);
  while (!remaining.empty()) {
    const auto comma = remaining.find(',');
    const auto token = remaining.substr(0, comma);
    int value{};
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value <= 0) {
      throw std::runtime_error(std::string("Invalid catalog id in field ") + field);
    }
    ids.insert(value);
    if (comma == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(comma + 1);
  }
  return ids;
}

MoneyType parse_currency(const std::string &name) {
  if (name == kGoldCurrency) {
    return MoneyType::Gold;
  }
  if (name == kTokenCurrency) {
    return MoneyType::HersanderToken;
  }
  throw std::runtime_error("Unknown Vehicle.priceType");
}

std::set<int> npc_ids(const std::vector<Json> &npcs) {
  std::set<int> ids;
  for (const auto &row : npcs) {
    ids.insert(fields::required_identifier(row, "id"));
  }
  return ids;
}

// The ConsumeItem requirement of one InteractionCondition id, when it has one.
std::optional<std::pair<int, int>> consumed_item(const std::vector<Json> &conditions,
                                                 int condition_id) {
  for (const auto &row : conditions) {
    if (fields::optional_identifier(row, "conditionId") != condition_id ||
        fields::text(row, "typeCase") != kConsumeItemCondition) {
      continue;
    }

    const auto requirement = braced_object(row, "type");
    return std::pair{positive_json_int(requirement, "itemId"),
                     positive_json_int(requirement, "count")};
  }
  return std::nullopt;
}

} // namespace

const std::vector<std::string> &CompanionCatalog::required_tables() {
  static const std::vector<std::string> tables = {
      "SummonNpc",   "Npc",      "Item",        "SkillEffect",         "Vehicle",
      "Interaction", "InteractionCondition",   "MateNpc", "MateIntimateLevel",
      "DoodadMateInteraction"};
  return tables;
}

std::shared_ptr<const CompanionCatalog> CompanionCatalog::load(ClientCatalog &catalog) {
  CompanionCatalogTables tables;
  tables.summon_npcs = catalog.rows("SummonNpc");
  tables.npcs = catalog.rows("Npc");
  tables.items = catalog.rows("Item");
  tables.skill_effects = catalog.rows("SkillEffect");
  tables.vehicles = catalog.rows("Vehicle");
  tables.interactions = catalog.rows("Interaction");
  tables.interaction_conditions = catalog.rows("InteractionCondition");
  tables.mate_npcs = catalog.rows("MateNpc");
  tables.mate_intimate_levels = catalog.rows("MateIntimateLevel");
  tables.doodad_mate_interactions = catalog.rows("DoodadMateInteraction");
  return std::make_shared<const CompanionCatalog>(tables);
}

CompanionCatalog::CompanionCatalog(const CompanionCatalogTables &tables) {
  const auto known_npcs = npc_ids(tables.npcs);

  for (const auto &row : tables.summon_npcs) {
    SummonDefinition summon;
    summon.id = fields::required_identifier(row, "id");
    summon.npc_cms_id = fields::required_identifier(row, "FK_npcId");
    summon.rideable = fields::text(row, "ability") == kMountAbility;
    if (!known_npcs.contains(summon.npc_cms_id)) {
      throw std::runtime_error("SummonNpc.FK_npcId names no Npc row");
    }
    if (!summons_.emplace(summon.id, summon).second) {
      throw std::runtime_error("Duplicate SummonNpc id");
    }
  }

  // registerSummonNpc effects, then the consumables that cast them.
  std::unordered_map<int, int> summon_by_effect;
  for (const auto &row : tables.skill_effects) {
    if (fields::text(row, "jsonCase") != kRegisterSummonEffect) {
      continue;
    }

    const auto effect_id = fields::required_identifier(row, "id");
    const auto summon_id = positive_json_int(braced_object(row, "json"), "cmsId");
    if (!summons_.contains(summon_id)) {
      throw std::runtime_error("registerSummonNpc names no SummonNpc row");
    }
    summon_by_effect.emplace(effect_id, summon_id);
  }
  for (const auto &row : tables.items) {
    for (const auto effect_id : skill_effect_list(row)) {
      const auto summon = summon_by_effect.find(effect_id);
      if (summon != summon_by_effect.end()) {
        const auto item_id = fields::required_identifier(row, "id");
        summon_by_item_.emplace(item_id, summon->second);
        const auto animation = fields::text(row, "FK_interactionAnimId");
        if (!animation.empty()) {
          registration_animations_.emplace(
              item_id, fields::required_identifier(row, "FK_interactionAnimId"));
        }
      }
    }
  }

  for (const auto &row : tables.vehicles) {
    VehicleDefinition vehicle;
    vehicle.id = fields::required_identifier(row, "id");
    const auto type = fields::text(row, "vehicleType");
    if (type == kTradeWagonType) {
      vehicle.kind = VehicleKind::trade_wagon;
    } else if (type == kShipType) {
      vehicle.kind = VehicleKind::ship;
    } else {
      throw std::runtime_error("Unknown Vehicle.vehicleType");
    }
    vehicle.max_load_capacity = fields::non_negative_int(row, "maxLoadCapacity");
    vehicle.price_currency = parse_currency(fields::text(row, "priceType"));
    vehicle.price = fields::non_negative_int(row, "priceValue");
    vehicle.max_health = fields::non_negative_int(row, "health");
    vehicle.interaction_group_id = fields::non_negative_int(row, "FK_interactionGroupId");
    if (vehicle.max_health <= 0) {
      throw std::runtime_error("Vehicle.health must be positive");
    }
    if (!vehicles_.emplace(vehicle.id, vehicle).second) {
      throw std::runtime_error("Duplicate Vehicle id");
    }
  }

  for (const auto &row : tables.interactions) {
    if (fields::text(row, "playerTaskCase") != kRepairWagonTask) {
      continue;
    }

    RepairWagonTask task;
    task.interaction_id = fields::required_identifier(row, "id");
    task.group_id = fields::required_identifier(row, "groupId");
    task.add_durability = positive_json_int(braced_object(row, "playerTask"), "addDurability");
    const auto condition_id = fields::optional_identifier(row, "FK_conditionId");
    if (condition_id) {
      const auto consumed = consumed_item(tables.interaction_conditions, *condition_id);
      if (consumed) {
        task.consumed_item_cms_id = consumed->first;
        task.consumed_item_count = consumed->second;
      }
    }
    repair_tasks_.emplace(task.interaction_id, task);
  }

  for (const auto &row : tables.mate_npcs) {
    MateDefinition mate;
    mate.id = fields::required_identifier(row, "id");
    mate.npc_cms_id = fields::required_identifier(row, "FK_npcId");
    if (!known_npcs.contains(mate.npc_cms_id)) {
      throw std::runtime_error("MateNpc.FK_npcId names no Npc row");
    }
    if (!mates_.emplace(mate.id, mate).second) {
      throw std::runtime_error("Duplicate MateNpc id");
    }
  }

  std::map<int, std::map<int, int>> levels_by_mate;
  for (const auto &row : tables.mate_intimate_levels) {
    const auto mate_id = fields::required_identifier(row, "FK_mateNpc");
    const auto level = fields::required_identifier(row, "level");
    const auto next_level = fields::non_negative_int(row, "nextLevelIntimate");
    if (!mates_.contains(mate_id) || !levels_by_mate[mate_id].emplace(level, next_level).second) {
      throw std::runtime_error("Invalid or duplicate MateIntimateLevel row");
    }
  }
  for (auto &[mate_id, levels] : levels_by_mate) {
    auto &definition = mates_.at(mate_id);
    int expected_level = 1;
    for (const auto &[level, next_level] : levels) {
      if (level != expected_level) {
        throw std::runtime_error("MateIntimateLevel levels must be contiguous from 1");
      }
      definition.next_level_intimacy.push_back(next_level);
      ++expected_level;
    }
  }

  for (const auto &row : tables.doodad_mate_interactions) {
    MateDoodadInteraction interaction;
    interaction.doodad_cms_id = fields::required_identifier(row, "FK_doodad");
    interaction.mate_ids = parenthesized_ids(row, "FK_mateNpcs");
    interaction.required_intimacy_level = fields::non_negative_int(row, "intimateLevel");
    interaction.probability_basis_points =
        std::min(fields::non_negative_int(row, "probability") * (kBasisPointsPerWhole / 100),
                 kBasisPointsPerWhole);
    mate_doodads_.emplace(interaction.doodad_cms_id, std::move(interaction));
  }
}

const SummonDefinition *CompanionCatalog::summon(int summon_npc_cms_id) const {
  const auto found = summons_.find(summon_npc_cms_id);
  return found == summons_.end() ? nullptr : &found->second;
}

std::optional<int> CompanionCatalog::registration_animation(int item_cms_id) const {
  const auto found = registration_animations_.find(item_cms_id);
  if (found == registration_animations_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::optional<int> CompanionCatalog::default_summon_id() const {
  std::optional<int> lowest_rideable_id;
  for (const auto &[summon_id, definition] : summons_) {
    if (!definition.rideable) {
      continue;
    }
    if (!lowest_rideable_id || summon_id < *lowest_rideable_id) {
      lowest_rideable_id = summon_id;
    }
  }
  return lowest_rideable_id;
}

std::optional<int> CompanionCatalog::summon_registered_by_item(int item_cms_id) const {
  const auto found = summon_by_item_.find(item_cms_id);
  if (found == summon_by_item_.end()) {
    return std::nullopt;
  }
  return found->second;
}

const VehicleDefinition *CompanionCatalog::vehicle(int vehicle_cms_id) const {
  const auto found = vehicles_.find(vehicle_cms_id);
  return found == vehicles_.end() ? nullptr : &found->second;
}

const RepairWagonTask *CompanionCatalog::repair_task(int interaction_id) const {
  const auto found = repair_tasks_.find(interaction_id);
  return found == repair_tasks_.end() ? nullptr : &found->second;
}

const MateDefinition *CompanionCatalog::mate(int mate_npc_cms_id) const {
  const auto found = mates_.find(mate_npc_cms_id);
  return found == mates_.end() ? nullptr : &found->second;
}

int CompanionCatalog::mate_intimacy_level(int mate_npc_cms_id, int intimacy) const {
  const auto *definition = mate(mate_npc_cms_id);
  if (!definition) {
    return 0;
  }

  // Level 1 starts at zero points; each nextLevelIntimate is the cost of the following level.
  int level = 1;
  std::int64_t threshold = 0;
  for (std::size_t index = 0; index + 1 < definition->next_level_intimacy.size(); ++index) {
    threshold += definition->next_level_intimacy[index];
    if (intimacy < threshold) {
      break;
    }
    ++level;
  }
  return level;
}

int CompanionCatalog::mate_intimacy_limit(int mate_npc_cms_id) const {
  const auto *definition = mate(mate_npc_cms_id);
  if (!definition) {
    return 0;
  }

  std::int64_t total = 0;
  for (const auto points : definition->next_level_intimacy) {
    total += points;
  }
  return static_cast<int>(std::min<std::int64_t>(total, std::numeric_limits<int>::max()));
}

const MateDoodadInteraction *CompanionCatalog::mate_doodad_interaction(int doodad_cms_id) const {
  const auto found = mate_doodads_.find(doodad_cms_id);
  return found == mate_doodads_.end() ? nullptr : &found->second;
}

std::size_t CompanionCatalog::summon_count() const {
  return summons_.size();
}

std::size_t CompanionCatalog::vehicle_count() const {
  return vehicles_.size();
}

std::size_t CompanionCatalog::mate_count() const {
  return mates_.size();
}

} // namespace chronicles::companion
