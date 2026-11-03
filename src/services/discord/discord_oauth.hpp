#pragma once

#include "auth/auth.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace chronicles {

struct DiscordOAuthConfig {
  std::string client_id;
  std::string client_secret;
  std::string redirect_uri;

  bool enabled() const;
  static DiscordOAuthConfig parse(std::string_view document);
  static DiscordOAuthConfig from_environment();
};

// External HTTPS is fixed to Discord. Transport injection is for offline tests;
// neither tokens nor the app secret are returned to a browser or launcher UI.
using DiscordRequest =
    std::function<nlohmann::json(const std::string &method, const std::string &path,
                                 const std::string &form, const std::string &bearer)>;

class DiscordOAuth {
public:
  explicit DiscordOAuth(
      AuthService &auth, DiscordOAuthConfig config = {}, DiscordRequest request = {},
      std::function<Clock::time_point()> now = [] { return Clock::now(); });

  bool enabled() const;
  nlohmann::json begin(unsigned short loopback_port, const std::string &link_session = {});
  std::string callback(const std::string &state, const std::string &code, const std::string &issuer,
                       bool denied = false);
  nlohmann::json poll(const std::string &request_id, const std::string &poll_secret,
                      const std::string &browser_proof = {});

private:
  struct Attempt {
    std::string poll_secret;
    std::string verifier;
    std::string browser_proof;
    unsigned short loopback_port{};
    std::string link_session;
    std::string verified_discord_id;
    Clock::time_point expires;
    bool callback_started{};
    nlohmann::json result{{"status", "pending"}};
  };

  void prune();

  AuthService &auth_;
  DiscordOAuthConfig config_;
  DiscordRequest request_;
  std::function<Clock::time_point()> now_;
  std::mutex mutex_;
  std::unordered_map<std::string, Attempt> attempts_;
};

std::string oauth_form_encode(std::string_view value);
std::unordered_map<std::string, std::string> oauth_query(std::string_view query);

} // namespace chronicles
