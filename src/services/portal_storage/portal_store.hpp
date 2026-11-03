#pragma once

#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

namespace chronicles {

class PortalStore {
public:
  explicit PortalStore(std::string connection = {});

  nlohmann::json list(const std::string &kind, bool include_drafts = false) const;

  void put(const std::string &kind, const nlohmann::json &entry);

  bool withdraw(const std::string &kind, const std::string &id);

private:
  std::string connection_;
  mutable std::mutex mutex_;
  nlohmann::json records_ = nlohmann::json::object();
};

} // namespace chronicles
