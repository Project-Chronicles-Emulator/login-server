#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace chronicles {

// Field readers for generated runtime profiles (tools/build_*_data.py output). Each reports
// the offending field name, so a bad profile fails at startup with an actionable message.

// A JSON integer that fits in int.
inline int profile_integer(const nlohmann::json &value, const char *field) {
  if (!value.is_number_integer()) {
    throw std::runtime_error(std::string("Profile field is not an integer: ") + field);
  }

  const auto number = value.get<std::int64_t>();
  if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
    throw std::runtime_error(std::string("Profile field is out of range: ") + field);
  }
  return static_cast<int>(number);
}

// A JSON integer that fits in std::int64_t.
inline std::int64_t profile_integer64(const nlohmann::json &value, const char *field) {
  if (!value.is_number_integer()) {
    throw std::runtime_error(std::string("Profile field is not an integer: ") + field);
  }
  return value.get<std::int64_t>();
}

// A finite percentage in (0, 100].
inline double profile_percent(const nlohmann::json &value, const char *field) {
  if (!value.is_number()) {
    throw std::runtime_error(std::string("Profile rate is not a number: ") + field);
  }

  constexpr double kAlwaysPercent = 100.0;
  const auto rate = value.get<double>();
  if (!std::isfinite(rate) || rate <= 0 || rate > kAlwaysPercent) {
    throw std::runtime_error(std::string("Profile rate is out of range: ") + field);
  }
  return rate;
}

} // namespace chronicles
