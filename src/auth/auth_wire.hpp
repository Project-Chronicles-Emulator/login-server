#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace chronicles {

inline constexpr std::size_t kMaximumAuthPayloadBytes = 1024 * 1024;
inline constexpr std::size_t kMaximumAuthWireBytes = 2 * kMaximumAuthPayloadBytes + 4096;
inline constexpr std::string_view kAuthWireAuthorization = "Chronicles-Service-v1";

class AuthWireError : public std::runtime_error {
public:
  AuthWireError() : std::runtime_error("Authentication service message verification failed") {}
};

struct AuthWireContext {
  std::string operation;
  std::string database_identity;
  std::string request_id;
  std::int64_t issued_at_ms{};
};

struct AuthWireRequest {
  AuthWireContext context;
  nlohmann::json envelope;
};

struct OpenAuthWireRequest {
  AuthWireContext context;
  nlohmann::json arguments;
};

AuthWireRequest seal_auth_request(const std::string &key, std::string_view operation,
                                  const std::string &database_identity,
                                  const nlohmann::json &arguments);

OpenAuthWireRequest open_auth_request(const std::string &key, std::string_view operation,
                                      const std::string &database_identity,
                                      const nlohmann::json &envelope);

nlohmann::json seal_auth_reply(const std::string &key, const AuthWireContext &context,
                               unsigned status, const nlohmann::json &reply);

nlohmann::json open_auth_reply(const std::string &key, const AuthWireContext &context,
                               unsigned status, const nlohmann::json &envelope);

// Only authenticated requests enter this bounded replay set. Retention exceeds twice
// the allowed wall-clock skew, so an accepted request cannot become fresh again.
class AuthReplayGuard {
public:
  using Clock = std::chrono::steady_clock;

  explicit AuthReplayGuard(
      std::size_t maximum = 131072, std::function<Clock::time_point()> now = [] {
        return Clock::now();
      });

  bool accept(const std::string &request_id);

private:
  std::size_t maximum_;
  std::function<Clock::time_point()> now_;
  std::mutex mutex_;
  std::unordered_set<std::string> accepted_;
  std::deque<std::pair<std::string, Clock::time_point>> order_;
};

} // namespace chronicles
