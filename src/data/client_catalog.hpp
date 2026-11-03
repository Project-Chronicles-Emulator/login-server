#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace chronicles {

// Static client definitions are loaded only when a table is used. The export
// manifest is checked at startup; each table is hashed before parsing.
class ClientCatalog {
public:
  explicit ClientCatalog(std::filesystem::path root);

  std::size_t table_count() const;

  // True when the verified manifest lists Tables/<table_name>.jsonl.
  bool has_table(const std::string &table_name) const;

  const std::string &source_executable_sha256() const;

  // Copy verified rows under the catalog lock for immutable startup indexes.
  std::vector<nlohmann::json> rows(const std::string &table_name);

  std::size_t row_count(const std::string &table_name);

  // Verified, sorted, positive and unique FogArea ids. Rejects empty or malformed tables.
  std::vector<int> fog_area_ids();

  std::optional<nlohmann::json> find_row(const std::string &table_name,
                                         const std::string &field_name,
                                         const std::string &value);

private:
  struct FileEntry {
    std::size_t bytes{};
    std::size_t records{};
    std::string sha256;
  };

  struct LoadedTable {
    std::vector<nlohmann::json> rows;
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> indexes;
  };

  LoadedTable &load_table(const std::string &table_name);

  std::filesystem::path root_;
  std::string source_executable_sha256_;
  std::unordered_map<std::string, FileEntry> files_;
  std::unordered_map<std::string, LoadedTable> tables_;
  std::mutex mutex_;
};

// Integer GameVariable.value for `name` when the row exists, parses as a base-10 integer and lies
// in [minimum, maximum]. Otherwise logs a warning prefixed with `subsystem` and returns nullopt,
// so the caller keeps its evidenced default. The GameVariable table must exist in the catalog.
std::optional<std::int64_t> game_variable_integer(ClientCatalog &catalog, std::string_view name,
                                                  std::int64_t minimum, std::int64_t maximum,
                                                  std::string_view subsystem);

// Base-10 integer stored as text in an exported catalog row field; nullopt when the field is
// absent, not text, or not an integer.
std::optional<std::int64_t> catalog_integer(const nlohmann::json &row, std::string_view field);

// True when an exported catalog row field holds the text "True".
bool catalog_flag(const nlohmann::json &row, std::string_view field);

// Text field of an exported catalog row; empty when absent or not text.
std::string catalog_text_field(const nlohmann::json &row, std::string_view field);

} // namespace chronicles
