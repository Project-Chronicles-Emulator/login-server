#pragma once

#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

namespace chronicles {

// Plain-text articles only. Empty connection strings select disposable memory storage.
class NewsStore {
public:
  explicit NewsStore(std::string connection = {});

  static void validate(const nlohmann::json &article);

  nlohmann::json list(bool include_drafts = false) const;

  void put(const nlohmann::json &article);

  bool withdraw(const std::string &id);

private:
  std::string connection_;
  mutable std::mutex mutex_;
  nlohmann::json articles_ = nlohmann::json::object();
};

} // namespace chronicles
