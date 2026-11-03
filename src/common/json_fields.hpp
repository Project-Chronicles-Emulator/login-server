#pragma once

// Typed field readers for operator-supplied JSON objects (admin API and saved settings).
// Each reader throws std::invalid_argument naming the field when its type or range is wrong,
// so validation messages stay consistent across callers.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace chronicles::json_fields {

// JSON numbers only: the string "100" is rejected rather than silently converted.
template <typename Integer>
std::optional<Integer> optional_integer(const nlohmann::json &object, const char *field) {
  const auto value = object.find(field);
  if (value == object.end() || value->is_null()) {
    return std::nullopt;
  }
  if (!value->is_number_integer()) {
    throw std::invalid_argument(std::string(field) + " must be an integer");
  }

  if (value->is_number_unsigned()) {
    const auto parsed = value->get<std::uint64_t>();
    if (parsed > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
      throw std::invalid_argument(std::string(field) + " is out of range");
    }
    return static_cast<Integer>(parsed);
  }

  const auto parsed = value->get<std::int64_t>();
  if (parsed < static_cast<std::int64_t>(std::numeric_limits<Integer>::min()) ||
      parsed > static_cast<std::int64_t>(std::numeric_limits<Integer>::max())) {
    throw std::invalid_argument(std::string(field) + " is out of range");
  }
  return static_cast<Integer>(parsed);
}

template <typename Integer>
Integer required_integer(const nlohmann::json &object, const char *field) {
  const auto value = optional_integer<Integer>(object, field);
  if (!value) {
    throw std::invalid_argument(std::string(field) + " is required");
  }
  return *value;
}

inline std::optional<std::string> optional_string(const nlohmann::json &object, const char *field) {
  const auto value = object.find(field);
  if (value == object.end() || value->is_null()) {
    return std::nullopt;
  }
  if (!value->is_string()) {
    throw std::invalid_argument(std::string(field) + " must be a string");
  }
  return value->get<std::string>();
}

inline std::string required_string(const nlohmann::json &object, const char *field) {
  auto value = optional_string(object, field);
  if (!value) {
    throw std::invalid_argument(std::string(field) + " is required");
  }
  return std::move(*value);
}

inline std::optional<bool> optional_bool(const nlohmann::json &object, const char *field) {
  const auto value = object.find(field);
  if (value == object.end() || value->is_null()) {
    return std::nullopt;
  }
  if (!value->is_boolean()) {
    throw std::invalid_argument(std::string(field) + " must be true or false");
  }
  return value->get<bool>();
}

inline std::optional<double> optional_number(const nlohmann::json &object, const char *field) {
  const auto value = object.find(field);
  if (value == object.end() || value->is_null()) {
    return std::nullopt;
  }
  if (!value->is_number()) {
    throw std::invalid_argument(std::string(field) + " must be a number");
  }
  return value->get<double>();
}

} // namespace chronicles::json_fields
