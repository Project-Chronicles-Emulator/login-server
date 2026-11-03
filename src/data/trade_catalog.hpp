#pragma once

// Static trade-run definitions built once at listener startup from the verified client catalog
// and the client's tradingpost.json map layers.
//
// Evidence (client CMS tables and data layers; see out/vehicle-research/trade-findings.md):
// - TradeGood, TradeGoodQuality, TradingPost, TradingPostGoods, TradingPostPurchase,
//   TradeBonusReward, TradePatternDesc, MakeTradeGood, DialogueSelection (TradeManagement).
// - GameVariable Trade* constants.
// - tradingpost.json (x5.client.data.TradingPostData): wagon parking/departure transforms and the
//   async sphere the client uses to request parking (0.9 x radius).
// Coordinates are converted from client centimeters to wire millimeters here.

#include "common/currency.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace chronicles {
class ClientCatalog;
} // namespace chronicles

namespace chronicles::trade {

// x5.cms.EX5TradeGoodQualityType values.
enum class GoodQuality : int {
  none = 0,
  better = 1,
  good = 2,
  normal = 3,
  bad = 4,
};

struct QualityTier {
  GoodQuality quality{GoodQuality::none};
  // TradeGoodQuality.overTime: milliseconds after production when this tier begins.
  std::int64_t begins_after_ms{};
  std::int64_t factor_basis_points{};
};

struct TradeGood {
  int id{};
  // x5.cms.EX5TradeGoodGroup value (None 0 .. Relic 6).
  int group{};
  std::int64_t basic_value{};
  // TradeGood.validPeriod in minutes: the lifetime of a voucher for this good.
  std::int64_t valid_period_minutes{};
  // Ordered by begins_after_ms; empty for goods without a quality table.
  std::vector<QualityTier> quality_tiers;
};

struct MaterialCost {
  int item_cms_id{};
  int count{};
};

// MakeTradeGood: the production fee and materials of a voucher (and the purchase price of a good
// loaded directly at its trading post).
struct ProductionCost {
  MoneyType currency{MoneyType::Gold};
  std::int64_t fee{};
  std::vector<MaterialCost> materials;
};

struct WorldPoint {
  int x_mm{};
  int y_mm{};
  int z_mm{};
};

struct TradingPost {
  int id{};
  int group{};
  // Placement from tradingpost.json; posts without one (test data) cannot start or park wagons.
  bool placed{};
  WorldPoint departure;
  float departure_yaw{};
  WorldPoint sphere_center;
  std::int64_t sphere_radius_mm{};
  // Map ids whose area contains the post (overworld channel maps).
  std::set<int> map_ids;
};

struct PurchaseReward {
  // An item reward (FK_Item x rewardItemAmount) or a money reward (moneyType x rewardBasisPoints of
  // the good's basic value); exactly one is set.
  std::optional<int> item_cms_id;
  int item_amount{};
  std::optional<MoneyType> currency;
  std::int64_t reward_basis_points{};
  std::optional<int> bonus_reward_id;
};

struct BonusReward {
  std::optional<int> item_cms_id;
  std::optional<MoneyType> currency;
  std::int64_t probability_basis_points{};
  std::int64_t minimum_amount{};
  std::int64_t maximum_amount{};
};

struct TradePattern {
  int id{};
  // Thousands digit of the id: 1 surging .. 5 plunging demand (data convention).
  int demand_tier{};
  // Hundreds digit of the id: the EX5TradeGoodGroup the description belongs to.
  int group{};
};

struct TradeSettings {
  int voucher_own_limit{10};
  std::int64_t refund_basis_points{1000};
  std::int64_t basic_market_min_percent{100};
  std::int64_t basic_market_max_percent{140};
  std::int64_t basic_market_step_basis_points{500};
  std::int64_t clamp_min_percent{50};
  std::int64_t clamp_max_percent{180};
  std::int64_t price_update_interval_minutes{60};
  std::int64_t wagon_respawn_cooldown_ms{30000};
};

// Raw rows. Kept separate from ClientCatalog so tests can build small catalogs.
struct TradeCatalogTables {
  std::vector<nlohmann::json> trade_goods;
  std::vector<nlohmann::json> trade_good_qualities;
  std::vector<nlohmann::json> trading_posts;
  std::vector<nlohmann::json> trading_post_goods;
  std::vector<nlohmann::json> trading_post_purchases;
  std::vector<nlohmann::json> trade_bonus_rewards;
  std::vector<nlohmann::json> trade_pattern_descs;
  std::vector<nlohmann::json> make_trade_goods;
  std::vector<nlohmann::json> dialogue_selections;
  std::vector<nlohmann::json> game_variables;
  std::vector<nlohmann::json> maps;
  // Parsed tradingpost.json documents ({"tradingPosts": [...]}).
  std::vector<nlohmann::json> trading_post_layers;
};

class TradeCatalog {
public:
  static const std::vector<std::string> &required_tables();

  // Reads every tradingpost.json under <catalog_root>/../raw/X5/Content/Data/BuiltData/Maps.
  static std::shared_ptr<const TradeCatalog> load(ClientCatalog &catalog,
                                                  const std::filesystem::path &catalog_root);

  explicit TradeCatalog(const TradeCatalogTables &tables);

  const TradeGood *good(int good_cms_id) const;

  const TradingPost *post(int trading_post_cms_id) const;

  const std::map<int, TradingPost> &posts() const;

  // True when `good_cms_id` is listed in TradingPostGoods for the post (loadable by purchase).
  bool post_sells(int trading_post_cms_id, int good_cms_id) const;

  const ProductionCost *production_cost(int good_cms_id) const;

  const PurchaseReward *purchase_reward(int destination_post, int good_cms_id,
                                        int origin_post) const;

  const std::vector<BonusReward> *bonus_reward(int bonus_reward_id) const;

  // Pattern ids for one demand tier and goods group, in id order.
  std::vector<int> patterns(int demand_tier, int group) const;

  const TradePattern *pattern(int pattern_id) const;

  // DialogueSelection ids whose PlayerTask is TradeManagement.
  bool is_trade_management_dialogue(int dialogue_cms_id) const;

  const TradeSettings &settings() const;

private:
  std::unordered_map<int, TradeGood> goods_;
  std::map<int, TradingPost> posts_;
  std::set<std::pair<int, int>> post_goods_;
  std::unordered_map<int, ProductionCost> production_costs_;
  std::map<std::tuple<int, int, int>, PurchaseReward> purchase_rewards_;
  std::unordered_map<int, std::vector<BonusReward>> bonus_rewards_;
  std::map<int, TradePattern> patterns_;
  std::set<int> trade_management_dialogues_;
  TradeSettings settings_;
};

} // namespace chronicles::trade
