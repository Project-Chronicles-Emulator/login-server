#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace chronicles {

// Only game-server infrastructure may call this interface. Player tokens are payloads,
// never the credential used to authenticate the private service connection.
class AuthAuthority {
public:
  virtual ~AuthAuthority() = default;

  virtual nlohmann::json call(std::string_view operation, const nlohmann::json &arguments) = 0;
};

// Initial deployments keep the private authority connection on the same host. The
// listener still requires an independent service secret; loopback alone is not identity.
std::shared_ptr<AuthAuthority> make_local_auth_authority(const std::string &origin,
                                                         const std::string &service_key,
                                                         const std::string &database_identity = {});

bool valid_auth_service_key(std::string_view key);

std::string read_auth_service_key(const std::string &path);

} // namespace chronicles
