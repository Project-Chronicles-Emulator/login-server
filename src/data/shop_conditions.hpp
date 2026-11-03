#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronicles {

// Facts about the acting character that catalog conditions may test. World sessions fill it
// from CompanyService (src/world/session/native_world.cpp); a default context describes a
// character without a company or pioneer permit.
struct ConditionContext {
  // The character's current company only, keyed by the catalog enum name (for example
  // "EX5CompanyType_BlueSalt") and mapped to its position level. Empty without a company.
  std::map<std::string, int> company_levels;
  bool has_pioneer_permit{};
  // Company of the trade good under evaluation, for CompanyLevelGreaterEqual conditions.
  std::optional<std::string> subject_company;
};

enum class ConditionResult {
  met,
  unmet,
  // The condition uses a kind the emulator cannot evaluate. Callers must fail closed.
  unsupported,
};

// Conditions from the InteractionCondition, ShopItemCondition and VoucherCondition tables.
// All three share the columns conditionId, group, logicalOperator, negated, typeCase and type.
//
// Semantics (Candidate beyond the shapes the merchant catalog uses): rows of one conditionId
// are partitioned by `group`; a group combines its rows with its logicalOperator (AND/OR) after
// applying `negated`; every group must hold. The merchant catalog only uses single-group
// conditions, so cross-group combination is unobserved.
class ConditionSet {
public:
  explicit ConditionSet(const std::vector<nlohmann::json> &rows);

  bool contains(int condition_id) const;

  // Unknown condition ids evaluate as unsupported rather than silently passing.
  ConditionResult evaluate(int condition_id, const ConditionContext &context) const;

  std::size_t size() const;

private:
  struct Row {
    std::string group;
    bool combine_with_and{};
    bool negated{};
    std::string type_case;
    nlohmann::json parameters;
  };

  std::unordered_map<int, std::vector<Row>> rows_by_condition_;
};

} // namespace chronicles
