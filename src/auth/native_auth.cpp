#include "auth/native_auth.hpp"
#include "distribution/distribution.hpp"

#include "p_c2a.pb.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <openssl/evp.h>

namespace chronicles {
namespace {

constexpr std::size_t      kMaximumNativeRequestBytes  = 65536;
constexpr std::size_t      kMaximumPresentedTokenBytes = 4096;
constexpr std::size_t      kMaximumNativeIdentityBytes = 256;
constexpr std::size_t      kIdentityAccountHashBytes  = 12;
constexpr std::size_t      kLaunchGrantHexLength      = 64;
constexpr unsigned         kMaximumNativeLoginAttempts = 120;
constexpr auto             kNativeLoginRateWindow      = std::chrono::seconds(60);
constexpr std::string_view kWorldEnterPrefix           = "/api/auth/WorldEnterReq/";
constexpr std::string_view kWorldGetPrefix             = "/api/auth/WorldGet/";
constexpr std::string_view kBearerPrefix               = "Bearer ";
constexpr std::string_view kLaunchPrefix               = "/launch/";

NativeHttpResponse protobuf_response(const google::protobuf::Message &message) {
  NativeHttpResponse response;
  if (!message.SerializeToString(&response.body)) {
    throw std::runtime_error("Cannot serialize native authentication response");
  }

  return response;
}

NativeHttpResponse http_error(unsigned status, const char *error) {
  NativeHttpResponse response;
  response.status       = status;
  response.content_type = "application/json";
  response.body         = nlohmann::json{{"error", error}}.dump();

  return response;
}

std::string authorization_token(std::string_view authorization) {
  if (!authorization.starts_with(kBearerPrefix) ||
      authorization.size() > kBearerPrefix.size() + kMaximumPresentedTokenBytes) {
    return {};
  }

  return std::string(authorization.substr(kBearerPrefix.size()));
}

std::optional<int> path_world_id(std::string_view target, std::string_view prefix) {
  const auto number   = target.substr(prefix.size());
  int        world_id = 0;
  const auto result   = std::from_chars(number.data(), number.data() + number.size(), world_id);
  if (result.ec != std::errc{} || result.ptr != number.data() + number.size() || world_id <= 0) {
    return std::nullopt;
  }

  return world_id;
}

std::string local_account_for_identity(std::string_view identity) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned digest_length{};
  if (EVP_Digest(identity.data(), identity.size(), digest.data(), &digest_length,
                 EVP_sha256(), nullptr) != 1 ||
      digest_length < kIdentityAccountHashBytes) {
    throw std::runtime_error("Cannot hash native identity");
  }

  const auto digest_prefix = std::string(
      reinterpret_cast<const char *>(digest.data()), kIdentityAccountHashBytes);
  return "native_" + hex_encode(digest_prefix);
}

bool is_launch_grant(std::string_view value) {
  return value.size() == kLaunchGrantHexLength &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

} // namespace

NativeAuth::NativeAuth(AuthService &service, NativeAuthConfig config)
  : service_(service), config_(std::move(config)) {
  if (!config_.enabled) {
    return;
  }

  while (config_.public_address.ends_with('/')) {
    config_.public_address.pop_back();
  }
  const auto origin = distribution::parse_service_origin(config_.public_address);
  if (!origin.loopback &&
      (!config_.require_launch_grant || config_.separate_accounts_by_identity)) {
    throw std::invalid_argument("Remote native authentication requires named-account launch grants");
  }
  if (config_.region.empty() ||
      (!config_.require_launch_grant &&
       (config_.local_account.empty() || config_.local_password.empty()))) {
    throw std::invalid_argument(
        "Native local authentication requires a region and, without launch grants, "
        "a configured account and password");
  }
}

NativeHttpResponse NativeAuth::bootstrap(
    std::optional<std::string_view> launch_grant) const {
  auto auth_address = config_.public_address;
  if (launch_grant) {
    auth_address += std::string(kLaunchPrefix) + std::string(*launch_grant);
  }

  NativeHttpResponse response;
  response.content_type = "application/json";
  response.body         =
      nlohmann::json{
          {"regions", nlohmann::json::array(
               {{{"name", config_.region}, {"auth_address", auth_address}}})}}
      .dump();

  return response;
}

bool NativeAuth::allow_login_attempt() {
  std::lock_guard lock(rate_mutex_);
  const auto      current_time = Clock::now();
  if (current_time - login_rate_window_ >= kNativeLoginRateWindow) {
    login_rate_window_ = current_time;
    login_attempts_    = 0;
  }

  return ++login_attempts_ <= kMaximumNativeLoginAttempts;
}

NativeHttpResponse NativeAuth::steam_login(
    std::string_view content_type, const std::string &body,
    std::optional<std::string_view> launch_grant) {
  if (content_type != "application/protobuf") {
    return http_error(415, "native_login_requires_protobuf");
  }

  x5::pc2a::AuthSteamReq request;
  if (!request.ParseFromString(body) || request.auth_token().empty()) {
    return http_error(400, "invalid_native_steam_request");
  }

  if (launch_grant) {
    auto granted_session = service_.consume_launch_grant(*launch_grant);
    if (!granted_session) {
      granted_session = service_.reconnect_launch_grant(*launch_grant);
    }
    if (!granted_session) {
      return http_error(401, "invalid_launch_grant");
    }

    return protobuf_response(*granted_session);
  }
  if (config_.require_launch_grant) {
    return http_error(401, "launch_grant_required");
  }

  // Identity is only an account namespace for local development. The platform
  // ticket is neither verified nor retained, so this mode is never production auth.
  auto account_name = config_.local_account;
  if (config_.separate_accounts_by_identity) {
    if (request.identity().empty() ||
        request.identity().size() > kMaximumNativeIdentityBytes) {
      return http_error(400, "invalid_native_identity");
    }

    account_name = local_account_for_identity(request.identity());
    service_.create_account(account_name, config_.local_password);
  }

  const auto response = service_.login(account_name, config_.local_password);
  return protobuf_response(response);
}

NativeHttpResponse NativeAuth::login_upgrade(const std::string &token,
                                             std::string_view   content_type,
                                             const std::string &body,
                                             bool launch_prefixed) {
  if (content_type != "application/protobuf") {
    return http_error(415, "native_login_requires_protobuf");
  }

  // An upgrade requires the preceding login's live session. The retained-token
  // fallback is intentionally not used here: expired or revoked sessions stay revoked.
  std::lock_guard lock(session_mapping_mutex_);
  if (!service_.authenticated(token)) {
    return http_error(401, "native_authentication_required");
  }

  x5::pc2a::LoginUpgradeReq request;
  if (!request.ParseFromString(body)) {
    return http_error(400, "invalid_native_login_upgrade_request");
  }

  // Explicit grants and per-identity sessions must retain their account across upgrade.
  // The legacy configured-account mode keeps its previous credential recheck behavior.
  if (launch_prefixed || config_.separate_accounts_by_identity ||
      config_.require_launch_grant) {
    const auto rotated = service_.rotate_session(token);
    if (!rotated) {
      return http_error(401, "native_authentication_required");
    }

    return protobuf_response(*rotated);
  }

  return protobuf_response(service_.login(config_.local_account, config_.local_password));
}

NativeHttpResponse NativeAuth::world_list(const std::string &token, std::optional<int> world_id) {
  auto response = service_.public_worlds();
  if (service_.authenticated(token)) {
    response = service_.worlds(token);
  }
  response.set_region(config_.region);
  if (world_id) {
    auto *worlds = response.mutable_worlds();
    for (int index = worlds->size() - 1; index >= 0; --index) {
      if (worlds->Get(index).world_id() != *world_id) {
        worlds->DeleteSubrange(index, 1);
      }
    }
    if (worlds->empty()) {
      response.set_error_code(x5::ec::InvalidWorld);
    }
  }

  for (auto &world : *response.mutable_worlds()) {
    if (!config_.client_version.empty()) {
      world.set_version(config_.client_version);
    }
    if (!config_.client_protocol.empty()) {
      world.set_protocol(config_.client_protocol);
    }
  }

  return protobuf_response(response);
}

bool NativeAuth::authenticate_session(const std::string &presented_token) {
  if (!config_.enabled || presented_token.empty() ||
      presented_token.size() > kMaximumPresentedTokenBytes) {
    return false;
  }

  std::lock_guard lock(session_mapping_mutex_);
  if (service_.authenticated(presented_token)) {
    return true;
  }
  if (config_.separate_accounts_by_identity || config_.require_launch_grant) {
    // A retained official token has no verified account binding in either mode.
    return false;
  }
  if (!allow_login_attempt()) {
    return false;
  }

  const auto login = service_.login(config_.local_account, config_.local_password);
  if (login.ec() != x5::ec::Success) {
    return false;
  }
  if (!service_.bind_local_session(login.access_token(), presented_token)) {
    service_.logout(login.access_token());
    return false;
  }

  return true;
}

std::optional<NativeHttpResponse>
NativeAuth::handle(std::string_view method, std::string_view target, std::string_view authorization,
                   std::string_view content_type, const std::string &body) {
  if (!config_.enabled) {
    return std::nullopt;
  }

  std::optional<std::string_view> launch_grant;
  if (target.starts_with(kLaunchPrefix)) {
    const auto prefixed_target = target.substr(kLaunchPrefix.size());
    const auto separator = prefixed_target.find('/');
    if (separator == std::string_view::npos ||
        !is_launch_grant(prefixed_target.substr(0, separator))) {
      return http_error(401, "invalid_launch_grant");
    }

    launch_grant = prefixed_target.substr(0, separator);
    target = prefixed_target.substr(separator);
    if (!target.starts_with("/api/auth/")) {
      return http_error(404, "native_route_not_implemented");
    }
  }
  if (target != "/native/bootstrap" && !target.starts_with("/api/auth/")) {
    return std::nullopt;
  }
  if (body.size() > kMaximumNativeRequestBytes) {
    return http_error(413, "native_request_too_large");
  }

  try {
    if ((target == "/native/bootstrap" || target == "/api/auth/BootstrapReq") && method == "GET") {
      if (launch_grant && !service_.valid_launch_grant(*launch_grant)) {
        return http_error(401, "invalid_launch_grant");
      }

      return bootstrap(launch_grant);
    }
    if (target == "/api/auth/steam/AuthReq" && method == "PUT") {
      return steam_login(content_type, body, launch_grant);
    }

    const auto token = authorization_token(authorization);
    if (target == "/api/auth/LoginUpgradeReq" && method == "PUT") {
      return login_upgrade(token, content_type, body, launch_grant.has_value());
    }
    if (target == "/api/auth/AuthLogoutReq" && method == "PUT") {
      // The native client does not parse a logout reply. An old platform token is expected when
      // switching an already running client to the local service; logout is therefore idempotent.
      service_.logout(token);
      return NativeHttpResponse{};
    }
    if (target == "/api/auth/WorldListReq" && method == "GET") {
      if (launch_grant && !service_.authenticated(token) &&
          !service_.valid_launch_grant(*launch_grant)) {
        return http_error(401, "invalid_launch_grant");
      }

      return world_list(token, std::nullopt);
    }
    if (target.starts_with(kWorldGetPrefix) && method == "GET") {
      const auto world_id = path_world_id(target, kWorldGetPrefix);
      if (!world_id) {
        return http_error(400, "invalid_world_id");
      }
      if (launch_grant && !service_.authenticated(token) &&
          !service_.valid_launch_grant(*launch_grant)) {
        return http_error(401, "invalid_launch_grant");
      }

      return world_list(token, world_id);
    }
    if (target.starts_with(kWorldEnterPrefix) && method == "PUT") {
      const auto world_id = path_world_id(target, kWorldEnterPrefix);
      if (!world_id) {
        return http_error(400, "invalid_world_id");
      }
      const auto authenticated = launch_grant ? service_.authenticated(token)
                                              : authenticate_session(token);
      if (!authenticated) {
        return http_error(401, "native_authentication_required");
      }

      x5::pb::authwebsocket::WorldEnterReq request;
      request.set_world_id(*world_id);
      return protobuf_response(service_.enter(token, request));
    }

    if (!service_.authenticated(token)) {
      return http_error(401, "native_authentication_required");
    }

    return http_error(404, "native_route_not_implemented");
  } catch (const Unauthorized &) {
    return http_error(401, "native_authentication_required");
  }
}

} // namespace chronicles
