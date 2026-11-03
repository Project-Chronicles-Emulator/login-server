#include "data/trade_catalog.hpp"

#include "common/catalog_fields.hpp"
#include "data/client_catalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace chronicles::trade {
namespace {

using Json = nlohmann::json;
namespace fields = catalog_fields;

constexpr std::string_view kTradeManagementTask = "TradeManagement";
constexpr std::string_view kGoldCurrency = "Gold";
constexpr std::string_view kTokenCurrency = "HersanderToken";
constexpr std::string_view kArcheWorldPath = "/Game/Maps/ArcheWorld/ArcheWorld.";
constexpr std::string_view kTradingPostFile = "tradingpost.json";
constexpr int kMillimetersPerCentimeter = 10;
constexpr int kMaterialColumns = 5;
constexpr std::size_t kMaximumTradingPostFiles = 64;
constexpr std::uintmax_t kMaximumTradingPostFileBytes = 1024 * 1024;

std::optional<MoneyType> optional_currency(const Json &row, const char *field) {
  const auto name = fields::text(row, field);
  if (name.empty()) {
    return std::nullopt;
  }
  if (name == kGoldCurrency) {
    return MoneyType::Gold;
  }
  if (name == kTokenCurrency) {
    return MoneyType::HersanderToken;
  }
  throw std::runtime_error(std::string("Unknown trade currency in field ") + field);
}

GoodQuality parse_quality(const std::string &name) {
  if (name == "Better") {
    return GoodQuality::better;
  }
  if (name == "Good") {
    return GoodQuality::good;
  }
  if (name == "Normal") {
    return GoodQuality::normal;
  }
  if (name == "Bad") {
    return GoodQuality::bad;
  }
  throw std::runtime_error("Unknown TradeGoodQuality.qualityType");
}

// EX5TradeGoodGroup names in enum order.
int parse_group(const std::string &name) {
  static const std::array<std::string_view, 7> kGroups = {
      "None", "Ingot", "Lumber", "Fabric", "Leather", "Archeum", "Relic"};
  for (std::size_t index = 0; index < kGroups.size(); ++index) {
    if (name == kGroups[index]) {
      return static_cast<int>(index);
    }
  }
  throw std::runtime_error("Unknown TradeGood.group");
}

double json_number(const Json &object, const char *field) {
  const auto found = object.find(field);
  if (found == object.end() || !found->is_number() || !std::isfinite(found->get<double>())) {
    throw std::runtime_error(std::string("Invalid tradingpost.json number ") + field);
  }
  return found->get<double>();
}

int centimeters_to_millimeters(double centimeters) {
  const auto millimeters = std::llround(centimeters * kMillimetersPerCentimeter);
  if (millimeters < std::numeric_limits<int>::min() ||
      millimeters > std::numeric_limits<int>::max()) {
    throw std::runtime_error("tradingpost.json coordinate out of range");
  }
  return static_cast<int>(millimeters);
}

WorldPoint point_from(const Json &position) {
  return {centimeters_to_millimeters(json_number(position, "x")),
          centimeters_to_millimeters(json_number(position, "y")),
          centimeters_to_millimeters(json_number(position, "z"))};
}

struct MapArea {
  int map_id{};
  double min_x{};
  double min_y{};
  double max_x{};
  double max_y{};
};

std::vector<MapArea> archeworld_areas(const std::vector<Json> &maps) {
  std::vector<MapArea> areas;
  for (const auto &row : maps) {
    if (fields::text(row, "world").find(kArcheWorldPath) == std::string::npos) {
      continue;
    }

    const auto area_text = fields::text(row, "area");
    const auto area = Json::parse("{" + area_text + "}", nullptr, false);
    if (area.is_discarded() || !area.contains("min") || !area.contains("max")) {
      throw std::runtime_error("Invalid Map.area");
    }
    areas.push_back({fields::required_identifier(row, "id"), json_number(area.at("min"), "x"),
                     json_number(area.at("min"), "y"), json_number(area.at("max"), "x"),
                     json_number(area.at("max"), "y")});
  }
  return areas;
}

std::int64_t game_variable(const std::vector<Json> &rows, std::string_view name,
                           std::int64_t fallback) {
  for (const auto &row : rows) {
    if (fields::text(row, "name") != name) {
      continue;
    }
    const auto value = fields::optional_number(row, "value");
    if (!value || *value < 0 || *value > static_cast<double>(std::numeric_limits<int>::max())) {
      throw std::runtime_error("Invalid trade GameVariable value");
    }
    return static_cast<std::int64_t>(*value);
  }
  return fallback;
}

std::string read_bounded_file(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > kMaximumTradingPostFileBytes) {
    throw std::runtime_error("tradingpost.json is unreadable or too large");
  }

  std::ifstream input(path, std::ios::binary);
  std::ostringstream content;
  content << input.rdbuf();
  if (!input.good() && !input.eof()) {
    throw std::runtime_error("Cannot read tradingpost.json");
  }
  return content.str();
}

} // namespace

const std::vector<std::string> &TradeCatalog::required_tables() {
  static const std::vector<std::string> tables = {
      "TradeGood",        "TradeGoodQuality", "TradingPost",      "TradingPostGoods",
      "TradingPostPurchase", "TradeBonusReward", "TradePatternDesc", "MakeTradeGood",
      "DialogueSelection", "GameVariable",     "Map"};
  return tables;
}

std::shared_ptr<const TradeCatalog> TradeCatalog::load(ClientCatalog &catalog,
                                                       const std::filesystem::path &catalog_root) {
  TradeCatalogTables tables;
  tables.trade_goods = catalog.rows("TradeGood");
  tables.trade_good_qualities = catalog.rows("TradeGoodQuality");
  tables.trading_posts = catalog.rows("TradingPost");
  tables.trading_post_goods = catalog.rows("TradingPostGoods");
  tables.trading_post_purchases = catalog.rows("TradingPostPurchase");
  tables.trade_bonus_rewards = catalog.rows("TradeBonusReward");
  tables.trade_pattern_descs = catalog.rows("TradePatternDesc");
  tables.make_trade_goods = catalog.rows("MakeTradeGood");
  tables.dialogue_selections = catalog.rows("DialogueSelection");
  tables.game_variables = catalog.rows("GameVariable");
  tables.maps = catalog.rows("Map");

  const auto maps_root = catalog_root.parent_path() / "raw" / "X5" / "Content" / "Data" /
                         "BuiltData" / "Maps";
  std::error_code error;
  if (std::filesystem::is_directory(maps_root, error) && !error) {
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(maps_root)) {
      if (entry.is_regular_file() && entry.path().filename() == kTradingPostFile) {
        if (files.size() == kMaximumTradingPostFiles) {
          throw std::runtime_error("Too many tradingpost.json files");
        }
        files.push_back(entry.path());
      }
    }
    std::sort(files.begin(), files.end());
    for (const auto &file : files) {
      tables.trading_post_layers.push_back(Json::parse(read_bounded_file(file)));
    }
  }
  return std::make_shared<const TradeCatalog>(tables);
}

TradeCatalog::TradeCatalog(const TradeCatalogTables &tables) {
  std::unordered_map<int, std::vector<QualityTier>> tiers_by_quality;
  for (const auto &row : tables.trade_good_qualities) {
    QualityTier tier;
    tier.quality = parse_quality(fields::text(row, "qualityType"));
    tier.begins_after_ms = fields::optional_integer(row, "overTime").value_or(0);
    tier.factor_basis_points = fields::required_integer(row, "qualityFactorBasisPoints");
    if (tier.begins_after_ms < 0 || tier.factor_basis_points < 0) {
      throw std::runtime_error("Invalid TradeGoodQuality row");
    }
    tiers_by_quality[fields::required_identifier(row, "id")].push_back(tier);
  }
  for (auto &[quality_id, tiers] : tiers_by_quality) {
    std::stable_sort(tiers.begin(), tiers.end(), [](const QualityTier &left,
                                                    const QualityTier &right) {
      return left.begins_after_ms < right.begins_after_ms;
    });
  }

  for (const auto &row : tables.trade_goods) {
    TradeGood good;
    good.id = fields::required_identifier(row, "id");
    good.group = parse_group(fields::text(row, "group"));
    good.basic_value = fields::non_negative_int(row, "basicValue");
    good.valid_period_minutes = fields::non_negative_int(row, "validPeriod");
    const auto quality_id = fields::optional_identifier(row, "FK_tradeGoodQualityId");
    if (quality_id) {
      const auto tiers = tiers_by_quality.find(*quality_id);
      if (tiers == tiers_by_quality.end()) {
        throw std::runtime_error("TradeGood names no TradeGoodQuality rows");
      }
      good.quality_tiers = tiers->second;
    }
    if (!goods_.emplace(good.id, std::move(good)).second) {
      throw std::runtime_error("Duplicate TradeGood id");
    }
  }

  for (const auto &row : tables.trading_posts) {
    TradingPost post;
    post.id = fields::required_identifier(row, "id");
    post.group = fields::non_negative_int(row, "group");
    if (!posts_.emplace(post.id, post).second) {
      throw std::runtime_error("Duplicate TradingPost id");
    }
  }

  const auto areas = archeworld_areas(tables.maps);
  for (const auto &layer : tables.trading_post_layers) {
    const auto entries = layer.find("tradingPosts");
    if (entries == layer.end() || !entries->is_array()) {
      throw std::runtime_error("tradingpost.json has no tradingPosts array");
    }
    for (const auto &entry : *entries) {
      const auto id = entry.value("cmsId", 0);
      auto post = posts_.find(id);
      if (post == posts_.end()) {
        throw std::runtime_error("tradingpost.json names an unknown TradingPost");
      }

      const auto &departure = entry.at("tradeWagonDeparturePos");
      const auto &sphere = entry.at("tradeWagonAsyncSphere");
      post->second.placed = true;
      post->second.departure = point_from(departure);
      post->second.departure_yaw = static_cast<float>(json_number(departure, "yaw"));
      post->second.sphere_center = point_from(sphere.at("center"));
      post->second.sphere_radius_mm =
          centimeters_to_millimeters(json_number(sphere, "radius"));
      const double x_cm = json_number(departure, "x");
      const double y_cm = json_number(departure, "y");
      for (const auto &area : areas) {
        if (x_cm >= area.min_x && x_cm <= area.max_x && y_cm >= area.min_y &&
            y_cm <= area.max_y) {
          post->second.map_ids.insert(area.map_id);
        }
      }
    }
  }

  for (const auto &row : tables.trading_post_goods) {
    const auto post_id = fields::required_identifier(row, "FK_tradingPostId");
    const auto good_id = fields::required_identifier(row, "FK_tradeGoodId");
    if (!posts_.contains(post_id) || !goods_.contains(good_id)) {
      throw std::runtime_error("TradingPostGoods names an unknown post or good");
    }
    post_goods_.emplace(post_id, good_id);
  }

  for (const auto &row : tables.make_trade_goods) {
    ProductionCost cost;
    const auto good_id = fields::required_identifier(row, "FK_TradeGood");
    cost.currency = optional_currency(row, "costType").value_or(MoneyType::Gold);
    cost.fee = fields::non_negative_int(row, "costValue");
    for (int column = 1; column <= kMaterialColumns; ++column) {
      const auto suffix = std::string(column < 10 ? "0" : "") + std::to_string(column);
      const auto item_field = "materialId" + suffix;
      const auto count_field = "materialCount" + suffix;
      const auto item = fields::optional_identifier(row, item_field.c_str());
      if (!item) {
        continue;
      }
      const auto count = fields::non_negative_int(row, count_field.c_str());
      if (count <= 0) {
        throw std::runtime_error("MakeTradeGood material count must be positive");
      }
      cost.materials.push_back({*item, count});
    }
    if (!goods_.contains(good_id) ||
        !production_costs_.emplace(good_id, std::move(cost)).second) {
      throw std::runtime_error("Invalid or duplicate MakeTradeGood row");
    }
  }

  for (const auto &row : tables.trading_post_purchases) {
    const auto destination = fields::required_identifier(row, "tradingPostId");
    const auto good_id = fields::required_identifier(row, "FK_tradeGoodId");
    const auto origin = fields::required_identifier(row, "startTradingPostId");
    PurchaseReward reward;
    reward.item_cms_id = fields::optional_identifier(row, "FK_Item");
    reward.item_amount = fields::non_negative_int(row, "rewardItemAmount");
    reward.currency = optional_currency(row, "moneyType");
    reward.reward_basis_points = fields::non_negative_int(row, "rewardBasisPoints");
    reward.bonus_reward_id = fields::optional_identifier(row, "FK_tradeBonusRewardId");
    const bool item_reward = reward.item_cms_id.has_value();
    const bool money_reward = reward.currency.has_value();
    if (item_reward == money_reward) {
      throw std::runtime_error("TradingPostPurchase must name one item or money reward");
    }
    // Only the unplaced test posts (4000/4100/7000) leave the item amount empty; they are read
    // as one item per unit (Candidate).
    if (item_reward && reward.item_amount == 0) {
      reward.item_amount = 1;
    }
    if (!purchase_rewards_.emplace(std::tuple{destination, good_id, origin}, reward).second) {
      throw std::runtime_error("Duplicate TradingPostPurchase row");
    }
  }

  for (const auto &row : tables.trade_bonus_rewards) {
    BonusReward bonus;
    bonus.item_cms_id = fields::optional_identifier(row, "FK_itemId");
    bonus.currency = optional_currency(row, "moneyType");
    bonus.probability_basis_points = fields::non_negative_int(row, "probBasisPoints");
    bonus.minimum_amount = fields::non_negative_int(row, "minAmount");
    bonus.maximum_amount = fields::non_negative_int(row, "maxAmount");
    if (bonus.item_cms_id.has_value() == bonus.currency.has_value() ||
        bonus.maximum_amount < bonus.minimum_amount) {
      throw std::runtime_error("Invalid TradeBonusReward row");
    }
    bonus_rewards_[fields::required_identifier(row, "id")].push_back(bonus);
  }

  for (const auto &row : tables.trade_pattern_descs) {
    TradePattern pattern;
    pattern.id = fields::required_identifier(row, "id");
    pattern.demand_tier = pattern.id / 1000;
    pattern.group = (pattern.id / 100) % 10;
    patterns_.emplace(pattern.id, pattern);
  }

  for (const auto &row : tables.dialogue_selections) {
    if (fields::text(row, "playerTaskCase") == kTradeManagementTask) {
      trade_management_dialogues_.insert(fields::required_identifier(row, "id"));
    }
  }

  const auto &variables = tables.game_variables;
  settings_.voucher_own_limit =
      static_cast<int>(game_variable(variables, "TradeVoucherOwnMaxLimit", 10));
  settings_.refund_basis_points = game_variable(variables, "TradeRefundBasisPoints", 1000);
  settings_.basic_market_min_percent = game_variable(variables, "TradeBasicMarketMinPercent", 100);
  settings_.basic_market_max_percent = game_variable(variables, "TradeBasicMarketMaxPercent", 140);
  settings_.basic_market_step_basis_points =
      game_variable(variables, "TradeBasicMarketIntervalBasisPoints", 500);
  settings_.clamp_min_percent = game_variable(variables, "TradeClampingMarketMinPercent", 50);
  settings_.clamp_max_percent = game_variable(variables, "TradeClampingMarketMaxPercent", 180);
  settings_.price_update_interval_minutes =
      game_variable(variables, "TradeMarketPriceUpdateIntervalPerMin", 60);
  settings_.wagon_respawn_cooldown_ms =
      game_variable(variables, "TradeWagonRespawnCooldown", 30000);
  if (settings_.basic_market_max_percent < settings_.basic_market_min_percent ||
      settings_.clamp_max_percent < settings_.clamp_min_percent ||
      settings_.basic_market_step_basis_points <= 0 ||
      settings_.price_update_interval_minutes <= 0) {
    throw std::runtime_error("Inconsistent trade market GameVariables");
  }
}

const TradeGood *TradeCatalog::good(int good_cms_id) const {
  const auto found = goods_.find(good_cms_id);
  return found == goods_.end() ? nullptr : &found->second;
}

const TradingPost *TradeCatalog::post(int trading_post_cms_id) const {
  const auto found = posts_.find(trading_post_cms_id);
  return found == posts_.end() ? nullptr : &found->second;
}

const std::map<int, TradingPost> &TradeCatalog::posts() const {
  return posts_;
}

bool TradeCatalog::post_sells(int trading_post_cms_id, int good_cms_id) const {
  return post_goods_.contains({trading_post_cms_id, good_cms_id});
}

const ProductionCost *TradeCatalog::production_cost(int good_cms_id) const {
  const auto found = production_costs_.find(good_cms_id);
  return found == production_costs_.end() ? nullptr : &found->second;
}

const PurchaseReward *TradeCatalog::purchase_reward(int destination_post, int good_cms_id,
                                                    int origin_post) const {
  const auto found = purchase_rewards_.find({destination_post, good_cms_id, origin_post});
  return found == purchase_rewards_.end() ? nullptr : &found->second;
}

const std::vector<BonusReward> *TradeCatalog::bonus_reward(int bonus_reward_id) const {
  const auto found = bonus_rewards_.find(bonus_reward_id);
  return found == bonus_rewards_.end() ? nullptr : &found->second;
}

std::vector<int> TradeCatalog::patterns(int demand_tier, int group) const {
  std::vector<int> matches;
  for (const auto &[id, pattern] : patterns_) {
    if (pattern.demand_tier == demand_tier && pattern.group == group) {
      matches.push_back(id);
    }
  }
  return matches;
}

const TradePattern *TradeCatalog::pattern(int pattern_id) const {
  const auto found = patterns_.find(pattern_id);
  return found == patterns_.end() ? nullptr : &found->second;
}

bool TradeCatalog::is_trade_management_dialogue(int dialogue_cms_id) const {
  return trade_management_dialogues_.contains(dialogue_cms_id);
}

const TradeSettings &TradeCatalog::settings() const {
  return settings_;
}

} // namespace chronicles::trade
