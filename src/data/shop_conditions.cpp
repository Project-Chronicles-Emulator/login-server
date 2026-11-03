#include "data/shop_conditions.hpp"

#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr const char *kPioneerPermitCase = "PioneerPermit";
constexpr const char *kCompanyCase = "Company";
constexpr const char *kCompanyLevelCase = "CompanyLevelGreaterEqual";
constexpr const char *kCompanyEnumPrefix = "EX5CompanyType_";
constexpr const char *kNoCompany = "EX5CompanyType_None";

std::string text_field(const Json &row, const char *field) {
  const auto found = row.find(field);
  if (found == row.end() || !found->is_string()) {
    return {};
  }
  return found->get<std::string>();
}

int parse_condition_id(const std::string &text) {
  int condition_id{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), condition_id);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      condition_id <= 0) {
    throw std::runtime_error("Invalid condition table conditionId");
  }
  return condition_id;
}

// The `type` column is the body of a JSON object, such as "level":3.
Json parse_parameters(const std::string &body) {
  auto parameters = Json::parse("{" + body + "}", nullptr, false);
  if (parameters.is_discarded() || !parameters.is_object()) {
    throw std::runtime_error("Invalid condition table type parameters");
  }
  return parameters;
}

bool is_integer_parameter(const Json &parameters, const char *name) {
  const auto found = parameters.find(name);
  return found != parameters.end() && found->is_number_integer() &&
         found->get<std::int64_t>() >= 0 &&
         found->get<std::int64_t>() <= std::numeric_limits<int>::max();
}

int company_level(const ConditionContext &context, const std::string &company) {
  const auto found = context.company_levels.find(company);
  return found == context.company_levels.end() ? 0 : found->second;
}

// RequirementType.Company, checked in the client's order (AAC_Requirement_CheckCompany_Candidate,
// dumps/ida_research/company-requirement-20261030): the character's company must equal
// companyId, a pioneer permit must be valid unless ignorePermit, and the position level must
// reach `level`. Omitted fields take their protobuf defaults (ignorePermit false, level 0).
// Candidate: "level" is a minimum; voucher condition 1003 lists levels 3 to 6 as alternatives,
// which is redundant under a minimum.
std::optional<bool> company_condition_met(const Json &parameters,
                                          const ConditionContext &context) {
  const auto company = parameters.find("companyId");
  if (company == parameters.end() || !company->is_string()) {
    return std::nullopt;
  }
  const auto ignore_permit = parameters.find("ignorePermit");
  if (ignore_permit != parameters.end() && !ignore_permit->is_boolean()) {
    return std::nullopt;
  }
  if (parameters.contains("level") && !is_integer_parameter(parameters, "level")) {
    return std::nullopt;
  }

  // ConditionContext lists only the character's current company; an empty map means none.
  const auto company_name = company->get<std::string>();
  const bool member = company_name == kNoCompany ? context.company_levels.empty()
                                                 : context.company_levels.contains(company_name);
  if (!member) {
    return false;
  }
  const bool permit_ignored = ignore_permit != parameters.end() && ignore_permit->get<bool>();
  if (!permit_ignored && !context.has_pioneer_permit) {
    return false;
  }
  const int required_level = parameters.value("level", 0);
  return company_level(context, company_name) >= required_level;
}

// Returns nullopt for a condition kind or parameter shape the emulator cannot evaluate.
std::optional<bool> evaluate_row(const std::string &type_case, const Json &parameters,
                                 const ConditionContext &context) {
  if (type_case == kPioneerPermitCase) {
    const auto permit = parameters.find("hasPermit");
    if (permit == parameters.end() || !permit->is_boolean()) {
      return std::nullopt;
    }
    return context.has_pioneer_permit == permit->get<bool>();
  }

  if (type_case == kCompanyCase) {
    return company_condition_met(parameters, context);
  }

  if (type_case == kCompanyLevelCase) {
    if (!context.subject_company || !is_integer_parameter(parameters, "level")) {
      return std::nullopt;
    }
    const auto company = std::string(kCompanyEnumPrefix) + *context.subject_company;
    return company_level(context, company) >= parameters.at("level").get<int>();
  }

  return std::nullopt;
}

} // namespace

ConditionSet::ConditionSet(const std::vector<Json> &rows) {
  for (const auto &row : rows) {
    Row parsed;
    const auto condition_id = parse_condition_id(text_field(row, "conditionId"));
    parsed.group = text_field(row, "group");
    const auto logical_operator = text_field(row, "logicalOperator");
    if (logical_operator != "AND" && logical_operator != "OR") {
      throw std::runtime_error("Unknown condition logicalOperator");
    }
    parsed.combine_with_and = logical_operator == "AND";
    parsed.negated = text_field(row, "negated") == "True";
    parsed.type_case = text_field(row, "typeCase");
    parsed.parameters = parse_parameters(text_field(row, "type"));
    rows_by_condition_[condition_id].push_back(std::move(parsed));
  }
}

bool ConditionSet::contains(int condition_id) const {
  return rows_by_condition_.contains(condition_id);
}

std::size_t ConditionSet::size() const {
  return rows_by_condition_.size();
}

ConditionResult ConditionSet::evaluate(int condition_id, const ConditionContext &context) const {
  const auto found = rows_by_condition_.find(condition_id);
  if (found == rows_by_condition_.end()) {
    return ConditionResult::unsupported;
  }

  // Group rows keep catalog order; a group's operator is the one its rows agree on.
  std::map<std::string, std::vector<const Row *>> groups;
  for (const auto &row : found->second) {
    groups[row.group].push_back(&row);
  }

  for (const auto &[group_name, rows] : groups) {
    const bool combine_with_and = rows.front()->combine_with_and;
    bool group_holds = combine_with_and;
    for (const auto *row : rows) {
      if (row->combine_with_and != combine_with_and) {
        return ConditionResult::unsupported;
      }

      auto holds = evaluate_row(row->type_case, row->parameters, context);
      if (!holds) {
        return ConditionResult::unsupported;
      }
      const bool row_result = row->negated ? !*holds : *holds;
      group_holds = combine_with_and ? (group_holds && row_result) : (group_holds || row_result);
    }

    if (!group_holds) {
      return ConditionResult::unmet;
    }
  }
  return ConditionResult::met;
}

} // namespace chronicles
