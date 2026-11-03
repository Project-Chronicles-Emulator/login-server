#pragma once

#include "auth/auth.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace chronicles {

struct NativeAuthConfig {
  bool enabled{false};
  std::string public_address{"http://127.0.0.1:8080"};
  std::string region{"local"};
  std::string local_account;
  std::string local_password;
  // Local-only development option: map the native identity string to a
  // separate emulator account. Steam tickets are still not verified.
  bool separate_accounts_by_identity{false};
  // Require a one-use launcher grant for initial native authentication.
  // This disables the shared-account and opaque-token fallback paths.
  bool require_launch_grant{false};
  std::string client_version;
  std::string client_protocol;
};

struct NativeHttpResponse {
  unsigned status{200};
  std::string content_type{"application/protobuf"};
  std::string body;
};

// Named-account launches consume one-use password-backed grants. Remote origins require this
// strict mode; legacy platform-identity mappings remain loopback-only development options.
class NativeAuth {
public:
  NativeAuth(AuthService &service, NativeAuthConfig config);

  // Existing clients may retain an opaque platform token when returning to the title screen.
  // Only this opt-in loopback adapter may bind it to authenticated local emulator credentials.
  bool authenticate_session(const std::string &presented_token);

  std::optional<NativeHttpResponse> handle(std::string_view method, std::string_view target,
                                           std::string_view authorization,
                                           std::string_view content_type, const std::string &body);

private:
  NativeHttpResponse bootstrap(std::optional<std::string_view> launch_grant) const;

  NativeHttpResponse steam_login(std::string_view content_type, const std::string &body,
                                 std::optional<std::string_view> launch_grant);

  NativeHttpResponse login_upgrade(const std::string &token, std::string_view content_type,
                                   const std::string &body, bool launch_prefixed);

  NativeHttpResponse world_list(const std::string &token, std::optional<int> world_id);

  bool allow_login_attempt();

  AuthService &service_;
  NativeAuthConfig config_;
  std::mutex rate_mutex_;
  std::mutex session_mapping_mutex_;
  Clock::time_point login_rate_window_{Clock::now()};
  unsigned login_attempts_{};
};

} // namespace chronicles
