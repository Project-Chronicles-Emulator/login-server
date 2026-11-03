#include "data/client_catalog.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <openssl/evp.h>

using namespace chronicles;

namespace {

class TemporaryCatalog {
public:
  TemporaryCatalog() {
    const auto unique_number = std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() /
           ("chronicles-client-catalog-test-" + std::to_string(unique_number));
    if (!std::filesystem::create_directory(root)) {
      throw std::runtime_error("Cannot create temporary client catalog directory");
    }
  }

  ~TemporaryCatalog() {
    if (root.parent_path() == std::filesystem::temp_directory_path()) {
      std::error_code ignored_error;
      std::filesystem::remove_all(root, ignored_error);
    }
  }

  std::filesystem::path root;
};

std::string sha256_hex(const std::string &value) {
  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned int digest_length{};
  EXPECT_EQ(EVP_Digest(value.data(), value.size(), digest, &digest_length, EVP_sha256(), nullptr),
            1);

  constexpr char kHexDigits[] = "0123456789abcdef";
  std::string hex;
  for (unsigned int index = 0; index < digest_length; ++index) {
    hex.push_back(kHexDigits[digest[index] >> 4]);
    hex.push_back(kHexDigits[digest[index] & 15]);
  }
  return hex;
}

TEST(ClientCatalog, LoadsIndexedRowsAndRejectsCorruption) {
  TemporaryCatalog temporary;
  const auto &root = temporary.root;
  std::filesystem::create_directories(root / "Tables");
  const std::string rows = "{\"id\":\"1\",\"name\":\"Test Sword\"}\n";

  {
    std::ofstream item_output(root / "Tables" / "Item.jsonl", std::ios::binary);
    item_output << rows;
  }

  nlohmann::json manifest = {
      {"format_version", 1},
      {"table_count", 1},
      {"files", nlohmann::json::array()},
  };
  manifest["files"].push_back({
      {"path", "Tables/Item.jsonl"},
      {"bytes", rows.size()},
      {"records", 1},
      {"sha256", sha256_hex(rows)},
  });
  {
    std::ofstream manifest_output(root / "manifest.json", std::ios::binary);
    manifest_output << manifest.dump();
  }

  ClientCatalog catalog(root);
  EXPECT_EQ(catalog.table_count(), 1);
  EXPECT_EQ(catalog.row_count("Item"), 1);
  EXPECT_EQ(catalog.find_row("Item", "id", "1")->at("name"), "Test Sword");
  EXPECT_FALSE(catalog.find_row("Item", "id", "2").has_value());

  {
    std::ofstream item_output(root / "Tables" / "Item.jsonl", std::ios::binary);
    item_output << "corrupt\n";
  }
  ClientCatalog corrupted_catalog(root);
  EXPECT_THROW(corrupted_catalog.row_count("Item"), std::runtime_error);
}

void write_fog_catalog(const std::filesystem::path &root,
                       const std::vector<nlohmann::json> &records) {
  std::filesystem::create_directories(root / "Tables");
  std::string rows;
  for (const auto &record : records) {
    rows += record.dump() + "\n";
  }
  {
    std::ofstream output(root / "Tables/FogArea.jsonl", std::ios::binary);
    output << rows;
  }

  const nlohmann::json entry = {
      {"path", "Tables/FogArea.jsonl"},
      {"bytes", rows.size()},
      {"records", records.size()},
      {"sha256", sha256_hex(rows)},
  };
  const nlohmann::json manifest = {
      {"format_version", 1},
      {"table_count", 1},
      {"files", nlohmann::json::array({entry})},
  };
  std::ofstream output(root / "manifest.json", std::ios::binary);
  output << manifest.dump();
}

TEST(ClientCatalog, FogAreaIdsAreSortedAndStrictlyValidated) {
  TemporaryCatalog temporary;
  write_fog_catalog(temporary.root, {{{"id", "30"}}, {{"id", "10"}}});
  ClientCatalog catalog(temporary.root);
  EXPECT_EQ(catalog.fog_area_ids(), (std::vector<int>{10, 30}));

  for (const auto *invalid : {"0", "-1", "2147483648", "10junk", ""}) {
    write_fog_catalog(temporary.root, {{{"id", invalid}}});
    ClientCatalog invalid_catalog(temporary.root);
    EXPECT_THROW(invalid_catalog.fog_area_ids(), std::runtime_error) << invalid;
  }

  for (const auto &invalid_row :
       {nlohmann::json{{"id", 10}}, nlohmann::json{{"name", "Missing id"}}}) {
    write_fog_catalog(temporary.root, {invalid_row});
    ClientCatalog invalid_catalog(temporary.root);
    EXPECT_THROW(invalid_catalog.fog_area_ids(), std::runtime_error);
  }

  write_fog_catalog(temporary.root, {{{"id", "10"}}, {{"id", "10"}}});
  ClientCatalog duplicate_catalog(temporary.root);
  EXPECT_THROW(duplicate_catalog.fog_area_ids(), std::runtime_error);

  write_fog_catalog(temporary.root, {});
  ClientCatalog empty_catalog(temporary.root);
  EXPECT_THROW(empty_catalog.fog_area_ids(), std::runtime_error);
}

} // namespace
