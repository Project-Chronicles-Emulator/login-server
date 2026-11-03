#pragma once

#include "common/currency.hpp"
#include "data/shop_conditions.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronicles {

class ClientCatalog;

// Shop multipliers and the default price scale are per mille.
inline constexpr std::int64_t kPerMille = 1000;

enum class ShopResetPeriod { never, daily, weekly, monthly };

// One Shop table row: an item offered by one shop and its per-mille price adjustments.
struct ShopGood {
  int item_cms_id{};
  std::int64_t price_multiplier_per_mille{};
  std::int64_t refund_multiplier_per_mille{};
  std::optional<int> limit_amount;
  ShopResetPeriod reset_period{ShopResetPeriod::never};
  // ShopItemCondition id (Shop.fk_shopItemCondition).
  std::optional<int> condition_id;
};

// Where an item lives in emulator storage.
enum class ShopItemStorage {
  unsupported,
  // WorldProgress::stackables, reported as ItemStackable.
  stackable,
  // WorldProgress::housing_items, reported as HousingItemCount.
  housing,
  // One CharacterItem row per unit, reported as Item.
  instance,
};

struct Price {
  MoneyType currency{MoneyType::Gold};
  std::int64_t amount{};
};

// Pricing, storage and wear facts from one Item table row.
struct ShopItemDefinition {
  int cms_id{};
  ShopItemStorage storage{ShopItemStorage::unsupported};
  // Item.priceType/priceValue; absent when the item has no price.
  std::optional<Price> price;
  // Item.refundType/refundValue; absent for non-sellable items (empty type or zero value).
  std::optional<Price> refund;
  int max_durability{};
  int grade{};
  // Item.npcRepairable is "True" and an ItemRepairCost row matches the item.
  bool npc_repairable{};
  // ItemRepairCost.costValue: the Gold cost to repair an item from zero durability.
  std::int64_t full_repair_cost{};
  // Item.weight per unit.
  std::int64_t unit_weight{};
};

enum class MerchantServiceKind { shop, repair, voucher_group };

// What one Interaction row asks of a merchant NPC.
struct MerchantService {
  MerchantServiceKind kind{MerchantServiceKind::shop};
  // Shop.id for shops, VoucherGroup id for voucher groups, zero for repairs.
  int target_id{};
  // Interaction.FK_conditionId.
  std::optional<int> condition_id;
};

struct TradeGoodDefinition {
  int id{};
  // TradeGood.basicValue.
  std::int64_t basic_value{};
  // TradeGood.validPeriod; the unit is unobserved and treated as hours (Candidate).
  std::int64_t valid_period{};
  // TradeGood.company without the EX5CompanyType_ prefix.
  std::string company;
};

struct VoucherOffer {
  int trade_good_id{};
  std::optional<int> condition_id;
};

// Raw catalog rows. Kept separate from ClientCatalog so tests can build small catalogs.
struct ShopCatalogTables {
  std::vector<nlohmann::json> items;
  std::vector<nlohmann::json> shops;
  std::vector<nlohmann::json> interactions;
  std::vector<nlohmann::json> npcs;
  std::vector<nlohmann::json> game_variables;
  std::vector<nlohmann::json> money;
  std::vector<nlohmann::json> interaction_conditions;
  std::vector<nlohmann::json> shop_item_conditions;
  std::vector<nlohmann::json> item_repair_costs;
  std::vector<nlohmann::json> voucher_groups;
  std::vector<nlohmann::json> voucher_conditions;
  std::vector<nlohmann::json> trade_goods;
};

// Immutable merchant indexes built once at listener startup. Construction rejects
// malformed rows instead of guessing prices.
class ShopCatalog {
public:
  // Catalog tables that must be present before load() is called.
  static const std::vector<std::string> &required_tables();

  static std::shared_ptr<const ShopCatalog> load(ClientCatalog &catalog);

  explicit ShopCatalog(const ShopCatalogTables &tables);

  // The merchant service an NPC offers through this interaction, when its interaction group
  // contains it.
  std::optional<MerchantService> service_for(int npc_cms_id, int interaction_id) const;

  bool is_merchant(int npc_cms_id) const;

  const ShopGood *good(int shop_id, int item_cms_id) const;

  const std::map<int, ShopGood> *goods(int shop_id) const;

  const ShopItemDefinition *item(int cms_id) const;

  const std::vector<VoucherOffer> *voucher_group(int group_id) const;

  const TradeGoodDefinition *trade_good(int trade_good_id) const;

  // Condition evaluation. Unknown or unevaluable conditions return unsupported.
  ConditionResult evaluate_interaction_condition(int condition_id,
                                                 const ConditionContext &context) const;
  ConditionResult evaluate_good_condition(int condition_id, const ConditionContext &context) const;
  ConditionResult evaluate_voucher_condition(int condition_id,
                                             const ConditionContext &context) const;

  // GameVariable.ItemMaxStack: the largest quantity accepted in one request line.
  int maximum_stack() const;

  // Money.limitUserMax for Gold.
  std::int64_t gold_limit() const;

  // GameVariable.BagMaxWeightMultiplier, read as a percentage of safe weight (Candidate).
  std::optional<std::int64_t> weight_ceiling_percent() const;

  // GameVariable.TradeRefundBasisPoints.
  std::optional<std::int64_t> trade_refund_basis_points() const;

  // First reset boundary after now; zero for limits that never reset.
  std::int64_t next_reset_epoch_ticks(ShopResetPeriod period, std::int64_t now_epoch_ticks) const;

  std::size_t shop_count() const;

private:
  std::unordered_map<int, std::map<int, ShopGood>> goods_by_shop_;
  std::unordered_map<int, ShopItemDefinition> items_;
  std::unordered_map<int, MerchantService> service_by_interaction_;
  std::unordered_map<int, std::set<int>> interactions_by_npc_;
  std::unordered_map<int, std::vector<VoucherOffer>> voucher_groups_;
  std::unordered_map<int, TradeGoodDefinition> trade_goods_;
  std::unique_ptr<ConditionSet> interaction_conditions_;
  std::unique_ptr<ConditionSet> shop_item_conditions_;
  std::unique_ptr<ConditionSet> voucher_conditions_;
  int maximum_stack_{};
  std::int64_t gold_limit_{};
  std::optional<std::int64_t> weight_ceiling_percent_;
  std::optional<std::int64_t> trade_refund_basis_points_;
  std::int64_t reset_base_epoch_ticks_{};
};

} // namespace chronicles
