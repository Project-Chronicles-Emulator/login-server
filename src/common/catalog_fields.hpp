#pragma once

// Typed readers for client catalog rows (data/generated/catalog/Tables/*.jsonl).
//
// Every catalog value is the original CSV text: numbers are decimal strings and an empty string
// means "not set". These readers convert at the consuming boundary and throw std::runtime_error
// naming the field when a present value is malformed, so a bad export fails at startup instead
// of producing guessed gameplay values.

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace chronicles::catalog_fields {

inline std::string text(const nlohmann::json &row, const char *field) {
  const auto found = row.find(field);
  if (found == row.end() || !found->is_string()) {
    return {};
  }
  return found->get<std::string>();
}

inline std::optional<std::int64_t> optional_integer(const nlohmann::json &row, const char *field) {
  const auto value = text(row, field);
  if (value.empty()) {
    return std::nullopt;
  }

  std::int64_t parsed{};
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    throw std::runtime_error(std::string("Invalid catalog integer in field ") + field);
  }
  return parsed;
}

inline std::int64_t required_integer(const nlohmann::json &row, const char *field) {
  const auto value = optional_integer(row, field);
  if (!value) {
    throw std::runtime_error(std::string("Missing catalog integer in field ") + field);
  }
  return *value;
}

// A positive 32-bit identifier (table ids and foreign keys).
inline std::optional<int> optional_identifier(const nlohmann::json &row, const char *field) {
  const auto value = optional_integer(row, field);
  if (!value) {
    return std::nullopt;
  }
  if (*value <= 0 || *value > std::numeric_limits<int>::max()) {
    throw std::runtime_error(std::string("Invalid catalog identifier in field ") + field);
  }
  return static_cast<int>(*value);
}

inline int required_identifier(const nlohmann::json &row, const char *field) {
  const auto value = optional_identifier(row, field);
  if (!value) {
    throw std::runtime_error(std::string("Missing catalog identifier in field ") + field);
  }
  return *value;
}

// A non-negative 32-bit amount; absent values read as `fallback`.
inline int non_negative_int(const nlohmann::json &row, const char *field, int fallback = 0) {
  const auto value = optional_integer(row, field);
  if (!value) {
    return fallback;
  }
  if (*value < 0 || *value > std::numeric_limits<int>::max()) {
    throw std::runtime_error(std::string("Catalog amount out of range in field ") + field);
  }
  return static_cast<int>(*value);
}

inline std::optional<double> optional_number(const nlohmann::json &row, const char *field) {
  const auto value = text(row, field);
  if (value.empty()) {
    return std::nullopt;
  }

  double parsed{};
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    throw std::runtime_error(std::string("Invalid catalog number in field ") + field);
  }
  return parsed;
}

// Catalog booleans are the strings "True" and "False"; empty reads as false.
inline bool flag(const nlohmann::json &row, const char *field) {
  const auto value = text(row, field);
  if (value.empty() || value == "False") {
    return false;
  }
  if (value == "True") {
    return true;
  }
  throw std::runtime_error(std::string("Invalid catalog boolean in field ") + field);
}

} // namespace chronicles::catalog_fields
