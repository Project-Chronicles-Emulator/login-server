#pragma once

// Static definitions for summoned mounts, vehicles and house mates, built once at listener
// startup from the verified client catalog.
//
// Evidence (client CMS tables, see out/vehicle-research/mounts-findings.md):
// - SummonNpc: every row has ability Mount; FK_npcId names the Npc row that is spawned.
// - SkillEffect jsonCase registerSummonNpc {"cmsId": N} registers summon N. Consumable items
//   reference those effects through Item.FK_skillEffectList.
// - Vehicle: one TradeWagon row (id 200) with price, health, load capacity, repair durability and
//   the interaction group that offers RepairWagon (Interaction 62, addDurability 1500, condition 62
//   = target DurabilityBelowThreshold AND caster ConsumeItem {itemId 140, count 1}).
// - MateNpc / MateIntimateLevel / DoodadMateInteraction for house mates.
// Construction rejects malformed rows instead of guessing values.

#include "common/currency.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronicles {
class ClientCatalog;
} // namespace chronicles

namespace chronicles::companion {

// One SummonNpc row. Only rows with ability Mount exist in this client build.
struct SummonDefinition {
  int id{};
  // Npc.id spawned for this summon (SummonNpc.FK_npcId).
  int npc_cms_id{};
  bool rideable{};
};

enum class VehicleKind {
  trade_wagon,
  ship,
};

struct VehicleDefinition {
  int id{};
  VehicleKind kind{VehicleKind::trade_wagon};
  // Vehicle.maxLoadCapacity: the most trade goods the vehicle carries.
  int max_load_capacity{};
  MoneyType price_currency{MoneyType::Gold};
  std::int64_t price{};
  // Vehicle.health: maximum hit points of a spawned vehicle.
  int max_health{};
  // Vehicle.FK_interactionGroupId: the interaction group a vehicle unit offers (RepairWagon).
  int interaction_group_id{};
  // Seats the client binds for this vehicle. The VehicleModel row of Vehicle 200 names only a
  // DriverSocket, so the emulator models one seat (Candidate).
  int seat_count{1};
};

// Interaction whose PlayerTask is RepairWagon.
struct RepairWagonTask {
  int interaction_id{};
  int group_id{};
  // PlayerTask.repairWagon.addDurability, added to the vehicle's current health.
  int add_durability{};
  // InteractionCondition ConsumeItem on the caster; absent when the condition has none.
  std::optional<int> consumed_item_cms_id;
  int consumed_item_count{};
};

struct MateDefinition {
  int id{};
  // Npc.id spawned for this mate (MateNpc.FK_npcId).
  int npc_cms_id{};
  // MateIntimateLevel.nextLevelIntimate for levels 1..N, in level order.
  std::vector<int> next_level_intimacy;
};

// DoodadMateInteraction: a housing object a mate reacts to.
struct MateDoodadInteraction {
  int doodad_cms_id{};
  std::set<int> mate_ids;
  int required_intimacy_level{};
  int probability_basis_points{};
};

// Raw catalog rows. Kept separate from ClientCatalog so tests can build small catalogs.
struct CompanionCatalogTables {
  std::vector<nlohmann::json> summon_npcs;
  std::vector<nlohmann::json> npcs;
  std::vector<nlohmann::json> items;
  std::vector<nlohmann::json> skill_effects;
  std::vector<nlohmann::json> vehicles;
  std::vector<nlohmann::json> interactions;
  std::vector<nlohmann::json> interaction_conditions;
  std::vector<nlohmann::json> mate_npcs;
  std::vector<nlohmann::json> mate_intimate_levels;
  std::vector<nlohmann::json> doodad_mate_interactions;
};

class CompanionCatalog {
public:
  // Catalog tables that must be present before load() is called.
  static const std::vector<std::string> &required_tables();

  static std::shared_ptr<const CompanionCatalog> load(ClientCatalog &catalog);

  explicit CompanionCatalog(const CompanionCatalogTables &tables);

  const SummonDefinition *summon(int summon_npc_cms_id) const;

  // The lowest rideable SummonNpc id: the horse `!mount` lends a character that has not
  // registered one. nullopt when the catalog has no rideable summon.
  std::optional<int> default_summon_id() const;

  // The summon a consumable registers when used, from its registerSummonNpc skill effect.
  std::optional<int> summon_registered_by_item(int item_cms_id) const;

  std::optional<int> registration_animation(int item_cms_id) const;

  const VehicleDefinition *vehicle(int vehicle_cms_id) const;

  // The RepairWagon task an interaction runs, when that interaction is one.
  const RepairWagonTask *repair_task(int interaction_id) const;

  const MateDefinition *mate(int mate_npc_cms_id) const;

  // 1-based intimacy level reached with `intimacy` points. Level 1 starts at zero points; an
  // unknown mate reports zero.
  int mate_intimacy_level(int mate_npc_cms_id, int intimacy) const;

  // The largest intimacy a mate can hold: the sum of every level's nextLevelIntimate.
  int mate_intimacy_limit(int mate_npc_cms_id) const;

  const MateDoodadInteraction *mate_doodad_interaction(int doodad_cms_id) const;

  std::size_t summon_count() const;
  std::size_t vehicle_count() const;
  std::size_t mate_count() const;

private:
  std::unordered_map<int, SummonDefinition> summons_;
  std::unordered_map<int, int> summon_by_item_;
  std::unordered_map<int, int> registration_animations_;
  std::unordered_map<int, VehicleDefinition> vehicles_;
  std::unordered_map<int, RepairWagonTask> repair_tasks_;
  std::unordered_map<int, MateDefinition> mates_;
  std::unordered_map<int, MateDoodadInteraction> mate_doodads_;
};

} // namespace chronicles::companion
