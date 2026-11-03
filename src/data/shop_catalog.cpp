#include "data/shop_catalog.hpp"

#include "data/client_catalog.hpp"
#include "cms.generated.pb.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <ratio>
#include <stdexcept>
#include <string_view>

namespace chronicles {
namespace {

using Json = nlohmann::json;

// Native epoch timestamps count 100 ns ticks since the Unix epoch.
using EpochTicks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;

constexpr std::string_view kShopTaskCase = "Shop";
constexpr std::string_view kRepairTaskCase = "Repair";
constexpr std::string_view kVoucherTaskCase = "VoucherGroup";
constexpr std::string_view kGoldCurrency = "Gold";
constexpr std::string_view kTokenCurrency = "HersanderToken";
constexpr std::string_view kEquipmentCategory = "Equipment";
constexpr std::string_view kHousingObjectCategory = "HousingObject";
// Buildings are placed from the same housing storage (UpdateHousingReq house_nubs consume one
// HousingBuilding item per building; docs/housing.md). Candidate: no capture shows a purchase.
constexpr std::string_view kHousingBuildingCategory = "HousingBuilding";
constexpr std::string_view kItemMaxStackVariable = "ItemMaxStack";
constexpr std::string_view kResetBaseVariable = "ResetBaseDateTime";
constexpr std::string_view kWeightMultiplierVariable = "BagMaxWeightMultiplier";
constexpr std::string_view kTradeRefundVariable = "TradeRefundBasisPoints";
constexpr std::string_view kItemGradePrefix = "EX5ItemGrade_";

// Categories the emulator stores as one WorldProgress stack per CMS ID. Observed as
// ItemsUpdateNoti.stackables in the official captures: Consumable, CraftMaterial,
// PolishingMaterial, Etc, TearsOfNui, WayPoint and Memo. Food and Repairer are the same kind of
// consumable but were not observed (Candidate). HousingPermit (item 123, the housing rent item
// of every HousingRentCost row) and HousingBuildingDrawing recipe materials are counted
// the same way (Candidate, docs/housing.md). A drawing is consumed by ItemCraft; it is not
// an installed HousingBuilding and must not enter housing-item storage.
// Recipe items remain inventory stacks until ItemUse consumes one and stores its learned craft.
constexpr std::array<std::string_view, 12> kStackableCategories = {"Consumable",
                                                                   "CraftMaterial",
                                                                   "Food",
                                                                   "PolishingMaterial",
                                                                   "Repairer",
                                                                   "Etc",
                                                                   "TearsOfNui",
                                                                   "WayPoint",
                                                                   "Memo",
                                                                   "HousingPermit",
                                                                   "HousingBuildingDrawing",
                                                                   "CraftRecipe"};

// Catalog numbers are decimal strings; an absent or empty field means "not set".
std::optional<std::int64_t> optional_catalog_integer(const Json &row, const char *field) {
  const auto found = row.find(field);
  if (found == row.end() || !found->is_string()) {
    return std::nullopt;
  }

  const auto &text = found->get_ref<const std::string &>();
  if (text.empty()) {
    return std::nullopt;
  }

  std::int64_t value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    throw std::runtime_error(std::string("Invalid catalog integer in field ") + field);
  }
  return value;
}

int required_catalog_identifier(const Json &row, const char *field) {
  const auto value = optional_catalog_integer(row, field);
  if (!value || *value <= 0 || *value > std::numeric_limits<int>::max()) {
    throw std::runtime_error(std::string("Missing or invalid catalog identifier in field ") +
                             field);
  }
  return static_cast<int>(*value);
}

std::optional<int> optional_catalog_identifier(const Json &row, const char *field) {
  if (!optional_catalog_integer(row, field)) {
    return std::nullopt;
  }
  return required_catalog_identifier(row, field);
}

std::string catalog_text(const Json &row, const char *field) {
  const auto found = row.find(field);
  if (found == row.end() || !found->is_string()) {
    return {};
  }
  return found->get<std::string>();
}

ShopItemStorage item_storage(const std::string &category) {
  if (category == kEquipmentCategory) {
    return ShopItemStorage::instance;
  }
  if (category == kHousingObjectCategory || category == kHousingBuildingCategory) {
    return ShopItemStorage::housing;
  }
  const bool stackable = std::ranges::any_of(
      kStackableCategories, [&](std::string_view name) { return category == name; });
  return stackable ? ShopItemStorage::stackable : ShopItemStorage::unsupported;
}

MoneyType parse_currency(const std::string &name) {
  if (name == kGoldCurrency) {
    return MoneyType::Gold;
  }
  if (name == kTokenCurrency) {
    return MoneyType::HersanderToken;
  }
  throw std::runtime_error("Unknown Item price or refund currency");
}

// An empty currency or value means "no price"; a refund of zero means "not sellable".
std::optional<Price> item_price(const Json &row, const char *type_field, const char *value_field,
                                bool zero_means_absent) {
  const auto currency_name = catalog_text(row, type_field);
  const auto amount = optional_catalog_integer(row, value_field);
  if (currency_name.empty() || !amount) {
    return std::nullopt;
  }
  if (*amount < 0) {
    throw std::runtime_error("Negative Item price or refund");
  }
  if (zero_means_absent && *amount == 0) {
    return std::nullopt;
  }
  return Price{parse_currency(currency_name), *amount};
}

std::string repair_cost_key(const Json &row, const char *slot_field) {
  return catalog_text(row, "category") + "|" + catalog_text(row, "equipmentType") + "|" +
         catalog_text(row, "grade") + "|" + catalog_text(row, slot_field);
}

std::unordered_map<std::string, std::int64_t> index_repair_costs(const std::vector<Json> &rows) {
  std::unordered_map<std::string, std::int64_t> costs;
  for (const auto &row : rows) {
    if (catalog_text(row, "costType") != kGoldCurrency) {
      continue;
    }
    const auto cost = optional_catalog_integer(row, "costValue");
    if (!cost || *cost < 0) {
      throw std::runtime_error("Invalid ItemRepairCost.costValue");
    }
    const auto inserted = costs.emplace(repair_cost_key(row, "equipType"), *cost);
    if (!inserted.second && inserted.first->second != *cost) {
      throw std::runtime_error("Conflicting ItemRepairCost rows for one item class");
    }
  }
  return costs;
}

ShopItemDefinition item_definition(const Json &row,
                                   const std::unordered_map<std::string, std::int64_t> &repairs) {
  ShopItemDefinition definition;
  definition.cms_id = required_catalog_identifier(row, "id");
  definition.storage = item_storage(catalog_text(row, "category"));
  definition.price = item_price(row, "priceType", "priceValue", false);
  definition.refund = item_price(row, "refundType", "refundValue", true);
  definition.unit_weight = optional_catalog_integer(row, "weight").value_or(0);
  if (definition.unit_weight < 0) {
    throw std::runtime_error("Negative Item.weight");
  }

  const auto durability = optional_catalog_integer(row, "maxDurability");
  if (durability) {
    if (*durability <= 0 || *durability > std::numeric_limits<int>::max()) {
      throw std::runtime_error("Invalid Item.maxDurability");
    }
    definition.max_durability = static_cast<int>(*durability);
  }

  const auto grade_name = catalog_text(row, "grade");
  if (!grade_name.empty()) {
    x5::cms::EX5ItemGrade grade{};
    if (!x5::cms::EX5ItemGrade_Parse(std::string(kItemGradePrefix) + grade_name, &grade)) {
      throw std::runtime_error("Unknown Item.grade");
    }
    definition.grade = static_cast<int>(grade);
  }

  if (catalog_text(row, "npcRepairable") == "True" && definition.max_durability > 0) {
    const auto cost = repairs.find(repair_cost_key(row, "equipSlot"));
    if (cost != repairs.end()) {
      definition.npc_repairable = true;
      definition.full_repair_cost = cost->second;
    }
  }
  return definition;
}

ShopResetPeriod parse_reset_period(const std::string &text) {
  // An empty period or "Once" is a lifetime limit for this character.
  if (text.empty() || text == "Once") {
    return ShopResetPeriod::never;
  }
  if (text == "Daily") {
    return ShopResetPeriod::daily;
  }
  if (text == "Weekly") {
    return ShopResetPeriod::weekly;
  }
  if (text == "Monthly") {
    return ShopResetPeriod::monthly;
  }
  throw std::runtime_error("Unknown Shop.resetPeriodType");
}

ShopGood shop_good(const Json &row) {
  ShopGood good;
  good.item_cms_id = required_catalog_identifier(row, "FK_itemId");
  good.price_multiplier_per_mille =
      optional_catalog_integer(row, "priceMultiplierPerMille").value_or(kPerMille);
  good.refund_multiplier_per_mille =
      optional_catalog_integer(row, "refundMultiplierPerMille").value_or(kPerMille);
  if (good.price_multiplier_per_mille < 0 || good.refund_multiplier_per_mille < 0) {
    throw std::runtime_error("Negative Shop multiplier");
  }

  const auto limit = optional_catalog_integer(row, "limitAmount");
  if (limit) {
    if (*limit <= 0 || *limit > std::numeric_limits<int>::max()) {
      throw std::runtime_error("Invalid Shop.limitAmount");
    }
    good.limit_amount = static_cast<int>(*limit);
  }

  good.reset_period = parse_reset_period(catalog_text(row, "resetPeriodType"));
  good.condition_id = optional_catalog_identifier(row, "fk_shopItemCondition");
  return good;
}

// Interaction.playerTask holds a JSON object body such as "shopId":1001.
int interaction_target_id(const Json &row, const char *key) {
  const auto task = Json::parse("{" + catalog_text(row, "playerTask") + "}", nullptr, false);
  if (task.is_discarded() || !task.is_object() || !task.contains(key) ||
      !task.at(key).is_number_integer() || task.at(key).get<std::int64_t>() <= 0 ||
      task.at(key).get<std::int64_t>() > std::numeric_limits<int>::max()) {
    throw std::runtime_error("Invalid merchant Interaction.playerTask");
  }
  return task.at(key).get<int>();
}

std::optional<MerchantService> interaction_service(const Json &row) {
  const auto task_case = catalog_text(row, "playerTaskCase");
  MerchantService service;
  if (task_case == kShopTaskCase) {
    service.kind = MerchantServiceKind::shop;
    service.target_id = interaction_target_id(row, "shopId");
  } else if (task_case == kVoucherTaskCase) {
    service.kind = MerchantServiceKind::voucher_group;
    service.target_id = interaction_target_id(row, "voucherGroupId");
  } else if (task_case == kRepairTaskCase) {
    service.kind = MerchantServiceKind::repair;
  } else {
    return std::nullopt;
  }
  service.condition_id = optional_catalog_identifier(row, "FK_conditionId");
  return service;
}

const Json *named_row(const std::vector<Json> &rows, const char *field, std::string_view value) {
  const auto found = std::ranges::find_if(
      rows, [&](const Json &row) { return catalog_text(row, field) == value; });
  return found == rows.end() ? nullptr : &*found;
}

// Text format "YYYY-MM-DD HH:MM:SS". The catalog records no time zone; UTC is assumed.
std::int64_t parse_reset_base_epoch_ticks(const std::string &text) {
  constexpr std::size_t kTimestampLength = 19;
  if (text.size() != kTimestampLength || text[4] != '-' || text[7] != '-' || text[10] != ' ' ||
      text[13] != ':' || text[16] != ':') {
    throw std::runtime_error("Invalid ResetBaseDateTime format");
  }

  const auto field = [&](std::size_t offset, std::size_t length) {
    int value{};
    const auto *begin = text.data() + offset;
    const auto parsed = std::from_chars(begin, begin + length, value);
    if (parsed.ec != std::errc{} || parsed.ptr != begin + length) {
      throw std::runtime_error("Invalid ResetBaseDateTime field");
    }
    return value;
  };

  const std::chrono::year_month_day date{std::chrono::year{field(0, 4)},
                                         std::chrono::month{static_cast<unsigned>(field(5, 2))},
                                         std::chrono::day{static_cast<unsigned>(field(8, 2))}};
  const int hour = field(11, 2);
  const int minute = field(14, 2);
  const int second = field(17, 2);
  if (!date.ok() || hour > 23 || minute > 59 || second > 59) {
    throw std::runtime_error("Invalid ResetBaseDateTime value");
  }

  const auto instant = std::chrono::sys_days{date} + std::chrono::hours{hour} +
                       std::chrono::minutes{minute} + std::chrono::seconds{second};
  return std::chrono::duration_cast<EpochTicks>(instant.time_since_epoch()).count();
}

std::optional<std::int64_t> game_variable(const std::vector<Json> &rows, std::string_view name) {
  const auto *row = named_row(rows, "name", name);
  return row ? optional_catalog_integer(*row, "value") : std::nullopt;
}

TradeGoodDefinition trade_good_definition(const Json &row) {
  TradeGoodDefinition good;
  good.id = required_catalog_identifier(row, "id");
  good.basic_value = optional_catalog_integer(row, "basicValue").value_or(0);
  good.valid_period = optional_catalog_integer(row, "validPeriod").value_or(0);
  good.company = catalog_text(row, "company");
  if (good.basic_value < 0 || good.valid_period < 0) {
    throw std::runtime_error("Negative TradeGood value or period");
  }
  return good;
}

} // namespace

const std::vector<std::string> &ShopCatalog::required_tables() {
  static const std::vector<std::string> tables = {
      "Item",           "Shop",         "Interaction",          "Npc",
      "GameVariable",   "Money",        "InteractionCondition", "ShopItemCondition",
      "ItemRepairCost", "VoucherGroup", "VoucherCondition",     "TradeGood"};
  return tables;
}

std::shared_ptr<const ShopCatalog> ShopCatalog::load(ClientCatalog &catalog) {
  ShopCatalogTables tables;
  tables.items = catalog.rows("Item");
  tables.shops = catalog.rows("Shop");
  tables.interactions = catalog.rows("Interaction");
  tables.npcs = catalog.rows("Npc");
  tables.game_variables = catalog.rows("GameVariable");
  tables.money = catalog.rows("Money");
  tables.interaction_conditions = catalog.rows("InteractionCondition");
  tables.shop_item_conditions = catalog.rows("ShopItemCondition");
  tables.item_repair_costs = catalog.rows("ItemRepairCost");
  tables.voucher_groups = catalog.rows("VoucherGroup");
  tables.voucher_conditions = catalog.rows("VoucherCondition");
  tables.trade_goods = catalog.rows("TradeGood");
  return std::make_shared<const ShopCatalog>(tables);
}

ShopCatalog::ShopCatalog(const ShopCatalogTables &tables)
    : interaction_conditions_(std::make_unique<ConditionSet>(tables.interaction_conditions)),
      shop_item_conditions_(std::make_unique<ConditionSet>(tables.shop_item_conditions)),
      voucher_conditions_(std::make_unique<ConditionSet>(tables.voucher_conditions)) {
  const auto repair_costs = index_repair_costs(tables.item_repair_costs);
  for (const auto &row : tables.items) {
    auto definition = item_definition(row, repair_costs);
    const int cms_id = definition.cms_id;
    if (!items_.emplace(cms_id, std::move(definition)).second) {
      throw std::runtime_error("Duplicate Item catalog row");
    }
  }

  for (const auto &row : tables.shops) {
    const int shop_id = required_catalog_identifier(row, "id");
    const auto good = shop_good(row);
    if (!goods_by_shop_[shop_id].emplace(good.item_cms_id, good).second) {
      throw std::runtime_error("Duplicate Shop item row");
    }
  }

  std::unordered_map<int, std::set<int>> interactions_by_group;
  for (const auto &row : tables.interactions) {
    auto service = interaction_service(row);
    if (!service) {
      continue;
    }
    const int interaction_id = required_catalog_identifier(row, "id");
    const int group_id = required_catalog_identifier(row, "groupId");
    if (!service_by_interaction_.emplace(interaction_id, *service).second) {
      throw std::runtime_error("Duplicate merchant Interaction row");
    }
    interactions_by_group[group_id].insert(interaction_id);
  }

  // Npc.interactionGroupId links an NPC to Interaction.groupId; zero means no group.
  for (const auto &row : tables.npcs) {
    const auto group_id = optional_catalog_integer(row, "interactionGroupId");
    if (!group_id || *group_id <= 0 || *group_id > std::numeric_limits<int>::max()) {
      continue;
    }
    const auto group = interactions_by_group.find(static_cast<int>(*group_id));
    if (group == interactions_by_group.end()) {
      continue;
    }
    const int npc_cms_id = required_catalog_identifier(row, "id");
    if (!interactions_by_npc_.emplace(npc_cms_id, group->second).second) {
      throw std::runtime_error("Duplicate Npc catalog row");
    }
  }

  for (const auto &row : tables.voucher_groups) {
    VoucherOffer offer;
    offer.trade_good_id = required_catalog_identifier(row, "FK_tradeGoodId");
    offer.condition_id = optional_catalog_identifier(row, "FK_VoucherConditionId");
    voucher_groups_[required_catalog_identifier(row, "VoucherGroupId")].push_back(offer);
  }
  for (const auto &row : tables.trade_goods) {
    auto good = trade_good_definition(row);
    const int good_id = good.id;
    if (!trade_goods_.emplace(good_id, std::move(good)).second) {
      throw std::runtime_error("Duplicate TradeGood catalog row");
    }
  }

  const auto stack_limit = game_variable(tables.game_variables, kItemMaxStackVariable);
  const auto *reset_row = named_row(tables.game_variables, "name", kResetBaseVariable);
  const auto *gold_row = named_row(tables.money, "type", kGoldCurrency);
  if (!stack_limit || !reset_row || !gold_row) {
    throw std::runtime_error("Missing ItemMaxStack, ResetBaseDateTime or Gold money row");
  }
  if (*stack_limit <= 0 || *stack_limit > std::numeric_limits<int>::max()) {
    throw std::runtime_error("Invalid ItemMaxStack");
  }
  maximum_stack_ = static_cast<int>(*stack_limit);
  reset_base_epoch_ticks_ = parse_reset_base_epoch_ticks(catalog_text(*reset_row, "value"));
  const auto gold_limit = optional_catalog_integer(*gold_row, "limitUserMax");
  if (!gold_limit || *gold_limit <= 0) {
    throw std::runtime_error("Invalid Gold Money.limitUserMax");
  }
  gold_limit_ = *gold_limit;
  weight_ceiling_percent_ = game_variable(tables.game_variables, kWeightMultiplierVariable);
  trade_refund_basis_points_ = game_variable(tables.game_variables, kTradeRefundVariable);
}

std::optional<MerchantService> ShopCatalog::service_for(int npc_cms_id, int interaction_id) const {
  const auto npc = interactions_by_npc_.find(npc_cms_id);
  if (npc == interactions_by_npc_.end() || !npc->second.contains(interaction_id)) {
    return std::nullopt;
  }
  return service_by_interaction_.at(interaction_id);
}

bool ShopCatalog::is_merchant(int npc_cms_id) const {
  return interactions_by_npc_.contains(npc_cms_id);
}

const ShopGood *ShopCatalog::good(int shop_id, int item_cms_id) const {
  const auto shop = goods_by_shop_.find(shop_id);
  if (shop == goods_by_shop_.end()) {
    return nullptr;
  }
  const auto good = shop->second.find(item_cms_id);
  return good == shop->second.end() ? nullptr : &good->second;
}

const std::map<int, ShopGood> *ShopCatalog::goods(int shop_id) const {
  const auto shop = goods_by_shop_.find(shop_id);
  return shop == goods_by_shop_.end() ? nullptr : &shop->second;
}

const ShopItemDefinition *ShopCatalog::item(int cms_id) const {
  const auto found = items_.find(cms_id);
  return found == items_.end() ? nullptr : &found->second;
}

const std::vector<VoucherOffer> *ShopCatalog::voucher_group(int group_id) const {
  const auto found = voucher_groups_.find(group_id);
  return found == voucher_groups_.end() ? nullptr : &found->second;
}

const TradeGoodDefinition *ShopCatalog::trade_good(int trade_good_id) const {
  const auto found = trade_goods_.find(trade_good_id);
  return found == trade_goods_.end() ? nullptr : &found->second;
}

ConditionResult ShopCatalog::evaluate_interaction_condition(int condition_id,
                                                            const ConditionContext &context) const {
  return interaction_conditions_->evaluate(condition_id, context);
}

ConditionResult ShopCatalog::evaluate_good_condition(int condition_id,
                                                     const ConditionContext &context) const {
  return shop_item_conditions_->evaluate(condition_id, context);
}

ConditionResult ShopCatalog::evaluate_voucher_condition(int condition_id,
                                                        const ConditionContext &context) const {
  return voucher_conditions_->evaluate(condition_id, context);
}

int ShopCatalog::maximum_stack() const {
  return maximum_stack_;
}

std::int64_t ShopCatalog::gold_limit() const {
  return gold_limit_;
}

std::optional<std::int64_t> ShopCatalog::weight_ceiling_percent() const {
  return weight_ceiling_percent_;
}

std::optional<std::int64_t> ShopCatalog::trade_refund_basis_points() const {
  return trade_refund_basis_points_;
}

std::size_t ShopCatalog::shop_count() const {
  return goods_by_shop_.size();
}

// Candidate reset grid: daily and weekly windows start at GameVariable.ResetBaseDateTime
// (a Monday 05:00) plus whole periods; monthly windows start on the first day of each UTC
// month at that time of day. No official limit reset was captured.
std::int64_t ShopCatalog::next_reset_epoch_ticks(ShopResetPeriod period,
                                                 std::int64_t now_epoch_ticks) const {
  using std::chrono::days;
  using std::chrono::floor;
  using std::chrono::months;
  using std::chrono::sys_days;
  using std::chrono::sys_time;
  using std::chrono::year_month_day;

  if (period == ShopResetPeriod::never) {
    return 0;
  }

  const sys_time<EpochTicks> now{EpochTicks{now_epoch_ticks}};
  const sys_time<EpochTicks> base{EpochTicks{reset_base_epoch_ticks_}};

  if (period == ShopResetPeriod::monthly) {
    const auto time_of_day = base - floor<days>(base);
    const year_month_day today{floor<days>(now)};
    const auto month_start = today.year() / today.month() / std::chrono::day{1};
    auto boundary = sys_days{month_start} + time_of_day;
    if (boundary <= now) {
      boundary = sys_days{month_start + months{1}} + time_of_day;
    }
    return boundary.time_since_epoch().count();
  }

  const auto interval = period == ShopResetPeriod::daily
                            ? std::chrono::duration_cast<EpochTicks>(days{1}).count()
                            : std::chrono::duration_cast<EpochTicks>(days{7}).count();
  const auto elapsed = now_epoch_ticks - reset_base_epoch_ticks_;
  auto completed_windows = elapsed / interval;
  // Floor division keeps instants before the base on the same grid.
  if (elapsed % interval < 0) {
    --completed_windows;
  }
  return reset_base_epoch_ticks_ + (completed_windows + 1) * interval;
}

} // namespace chronicles
