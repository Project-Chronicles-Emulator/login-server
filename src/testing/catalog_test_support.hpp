#pragma once

// A minimal verified ClientCatalog root for unit tests: each table is written as JSONL and the
// manifest records its size, row count and SHA-256, so ClientCatalog accepts it.

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace chronicles::testing_support {

class TestCatalog {
public:
  explicit TestCatalog(const std::map<std::string, std::vector<nlohmann::json>> &tables)
      : root_(create_unique_root()) {
    std::filesystem::create_directories(root_ / "Tables");
    auto files = nlohmann::json::array();
    for (const auto &[table, rows] : tables) {
      std::string body;
      for (const auto &row : rows) {
        body += row.dump() + "\n";
      }

      const auto relative = "Tables/" + table + ".jsonl";
      write(root_ / relative, body);
      files.push_back({{"path", relative},
                       {"bytes", body.size()},
                       {"records", rows.size()},
                       {"sha256", sha256_hex(body)}});
    }

    const nlohmann::json manifest{{"format_version", 1},
                                  {"source_executable_dump_sha256", "test"},
                                  {"table_count", tables.size()},
                                  {"files", files}};
    write(root_ / "manifest.json", manifest.dump());
  }

  TestCatalog(const TestCatalog &) = delete;

  TestCatalog &operator=(const TestCatalog &) = delete;

  ~TestCatalog() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  const std::filesystem::path &root() const {
    return root_;
  }

private:
  static std::filesystem::path create_unique_root() {
    const auto temporary_directory = std::filesystem::temp_directory_path();
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
      const auto candidate = temporary_directory /
          ("chronicles-catalog-test-" + std::to_string(random()));
      if (std::filesystem::create_directory(candidate)) {
        return candidate;
      }
    }
    throw std::runtime_error("Could not create a unique test catalog directory");
  }

  static void write(const std::filesystem::path &path, const std::string &body) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << body;
    if (!file) {
      throw std::runtime_error("Could not write test catalog file");
    }
  }

  static std::string sha256_hex(const std::string &bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &digest_length, EVP_sha256(), nullptr) != 1) {
      throw std::runtime_error("Could not hash test catalog file");
    }

    constexpr char kAlphabet[] = "0123456789abcdef";
    std::string hex;
    for (unsigned int index = 0; index < digest_length; ++index) {
      hex.push_back(kAlphabet[digest[index] >> 4]);
      hex.push_back(kAlphabet[digest[index] & 0x0f]);
    }
    return hex;
  }

  std::filesystem::path root_;
};

} // namespace chronicles::testing_support
