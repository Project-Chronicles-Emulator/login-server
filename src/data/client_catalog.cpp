#include "data/client_catalog.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <openssl/evp.h>
#include <spdlog/spdlog.h>

namespace chronicles {
namespace {

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Cannot open client catalog file: " + path.string());
  }

  return std::string(std::istreambuf_iterator<char>(input), {});
}

std::string sha256_hex(std::string_view bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_length{};
  const auto succeeded = EVP_Digest(bytes.data(), bytes.size(), digest.data(),
                                    &digest_length, EVP_sha256(), nullptr);
  if (succeeded != 1) {
    throw std::runtime_error("Cannot hash client catalog file");
  }

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < digest_length; ++index) {
    output << std::setw(2) << static_cast<unsigned>(digest[index]);
  }
  return output.str();
}

bool safe_relative_path(const std::filesystem::path &path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }

  for (const auto &component : path) {
    if (component == ".." || component == ".") {
      return false;
    }
  }
  return true;
}

std::string table_path(const std::string &table_name) {
  if (table_name.empty()) {
    throw std::invalid_argument("Client catalog table name is empty");
  }

  for (const char character : table_name) {
    const bool is_letter = (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z');
    const bool is_digit = character >= '0' && character <= '9';
    if (!is_letter && !is_digit && character != '_') {
      throw std::invalid_argument("Invalid client catalog table name");
    }
  }

  return "Tables/" + table_name + ".jsonl";
}

} // namespace

ClientCatalog::ClientCatalog(std::filesystem::path root) : root_(std::move(root)) {
  const auto manifest_path = root_ / "manifest.json";
  const auto manifest = nlohmann::json::parse(read_file(manifest_path));
  if (manifest.at("format_version") != 1) {
    throw std::runtime_error("Unsupported client catalog manifest version");
  }
  source_executable_sha256_ = manifest.value("source_executable_dump_sha256", std::string{});

  const auto &entries = manifest.at("files");
  if (!entries.is_array()) {
    throw std::runtime_error("Client catalog manifest files must be an array");
  }

  for (const auto &entry : entries) {
    const auto relative_path = std::filesystem::path(entry.at("path").get<std::string>());
    if (!safe_relative_path(relative_path)) {
      throw std::runtime_error("Unsafe client catalog manifest path");
    }

    FileEntry file;
    file.bytes = entry.at("bytes").get<std::size_t>();
    file.sha256 = entry.at("sha256").get<std::string>();
    if (!entry.at("records").is_null()) {
      file.records = entry.at("records").get<std::size_t>();
    }

    const auto inserted = files_.emplace(relative_path.generic_string(),
                                         std::move(file)).second;
    if (!inserted) {
      throw std::runtime_error("Duplicate client catalog manifest path");
    }
  }

  if (table_count() != manifest.at("table_count").get<std::size_t>()) {
    throw std::runtime_error("Client catalog manifest table count is invalid");
  }
}

std::size_t ClientCatalog::table_count() const {
  std::size_t count{};
  for (const auto &[path, file] : files_) {
    if (path.starts_with("Tables/")) {
      ++count;
    }
  }
  return count;
}

const std::string &ClientCatalog::source_executable_sha256() const {
  return source_executable_sha256_;
}

bool ClientCatalog::has_table(const std::string &table_name) const {
  return files_.contains(table_path(table_name));
}

ClientCatalog::LoadedTable &ClientCatalog::load_table(const std::string &table_name) {
  const auto path = table_path(table_name);
  if (auto existing = tables_.find(path); existing != tables_.end()) {
    return existing->second;
  }

  const auto entry = files_.find(path);
  if (entry == files_.end()) {
    throw std::invalid_argument("Client catalog table is absent: " + table_name);
  }

  const auto bytes = read_file(root_ / path);
  if (bytes.size() != entry->second.bytes || sha256_hex(bytes) != entry->second.sha256) {
    throw std::runtime_error("Client catalog hash mismatch: " + path);
  }

  LoadedTable loaded;
  std::istringstream input(bytes);
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      throw std::runtime_error("Blank client catalog row: " + path);
    }

    auto row = nlohmann::json::parse(line);
    if (!row.is_object()) {
      throw std::runtime_error("Client catalog row is not an object: " + path);
    }
    loaded.rows.push_back(std::move(row));
  }

  if (loaded.rows.size() != entry->second.records) {
    throw std::runtime_error("Client catalog row count mismatch: " + path);
  }

  auto [position, inserted] = tables_.emplace(path, std::move(loaded));
  if (!inserted) {
    throw std::runtime_error("Duplicate loaded client catalog table");
  }
  return position->second;
}

std::vector<nlohmann::json> ClientCatalog::rows(const std::string &table_name) {
  std::lock_guard lock(mutex_);
  return load_table(table_name).rows;
}

std::size_t ClientCatalog::row_count(const std::string &table_name) {
  std::lock_guard lock(mutex_);
  return load_table(table_name).rows.size();
}

std::optional<nlohmann::json> ClientCatalog::find_row(const std::string &table_name,
                                                       const std::string &field_name,
                                                       const std::string &value) {
  std::lock_guard lock(mutex_);
  auto &table = load_table(table_name);
  auto [index, inserted] = table.indexes.try_emplace(field_name);
  if (inserted) {
    for (std::size_t row_number = 0; row_number < table.rows.size(); ++row_number) {
      const auto &row = table.rows[row_number];
      if (row.contains(field_name) && row[field_name].is_string()) {
        index->second.try_emplace(row[field_name].get<std::string>(), row_number);
      }
    }
  }

  const auto matching = index->second.find(value);
  if (matching == index->second.end()) {
    return std::nullopt;
  }
  return table.rows[matching->second];
}

std::vector<int> ClientCatalog::fog_area_ids() {
  const auto area_rows = rows("FogArea");
  if (area_rows.empty()) {
    throw std::runtime_error("FogArea catalog is empty");
  }

  std::vector<int> ids;
  ids.reserve(area_rows.size());
  for (const auto &row : area_rows) {
    const auto field = row.find("id");
    if (field == row.end() || !field->is_string()) {
      throw std::runtime_error("FogArea catalog id must be text");
    }
    const auto &text = field->get_ref<const std::string &>();
    int area_id{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), area_id);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || area_id <= 0) {
      throw std::runtime_error("FogArea catalog has an invalid id");
    }
    ids.push_back(area_id);
  }

  std::ranges::sort(ids);
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
    throw std::runtime_error("FogArea catalog has duplicate ids");
  }
  return ids;
}

std::optional<std::int64_t> game_variable_integer(ClientCatalog &catalog, std::string_view name,
                                                  std::int64_t minimum, std::int64_t maximum,
                                                  std::string_view subsystem) {
  const auto row = catalog.find_row("GameVariable", "name", std::string(name));
  if (!row) {
    spdlog::warn("{}: GameVariable {} is missing; using default", subsystem, name);
    return std::nullopt;
  }

  // Exported rows store every value as text; integers are accepted for hand-built test rows.
  const auto value = row->value("value", nlohmann::json());
  std::optional<std::int64_t> parsed;
  if (value.is_number_integer()) {
    parsed = value.get<std::int64_t>();
  } else {
    parsed = catalog_integer(*row, "value");
  }

  if (!parsed || *parsed < minimum || *parsed > maximum) {
    spdlog::warn("{}: GameVariable {} is not an integer in [{}, {}]; using default", subsystem,
                 name, minimum, maximum);
    return std::nullopt;
  }
  return parsed;
}

std::optional<std::int64_t> catalog_integer(const nlohmann::json &row, std::string_view field) {
  const auto found = row.find(field);
  if (found == row.end() || !found->is_string()) {
    return std::nullopt;
  }

  const auto &text = found->get_ref<const std::string &>();
  std::int64_t number = 0;
  const auto *end = text.data() + text.size();
  const auto [last, error] = std::from_chars(text.data(), end, number);
  if (text.empty() || error != std::errc{} || last != end) {
    return std::nullopt;
  }
  return number;
}

bool catalog_flag(const nlohmann::json &row, std::string_view field) {
  return catalog_text_field(row, field) == "True";
}

std::string catalog_text_field(const nlohmann::json &row, std::string_view field) {
  const auto found = row.find(field);
  if (found == row.end() || !found->is_string()) {
    return {};
  }
  return found->get<std::string>();
}

} // namespace chronicles
