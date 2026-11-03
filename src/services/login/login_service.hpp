#pragma once

#include "auth/auth.hpp"
#include "auth/auth_admission.hpp"
#include "auth/auth_wire.hpp"
#include "auth/native_auth.hpp"
#include "distribution/distribution.hpp"
#include "services/discord/discord_oauth.hpp"
#include "services/news/news.hpp"
#include "services/portal_storage/portal_store.hpp"

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <memory>

namespace chronicles {

struct LoginListenerOptions {
  std::string bind_address{"127.0.0.1"};
  unsigned short port{8080};
  unsigned short authority_port{8090};
  std::string certificate_file;
  std::string private_key_file;
  unsigned maximum_connections{1280};
};

class LoginRouter {
public:
  using Request = boost::beast::http::request<boost::beast::http::string_body>;
  using Response = boost::beast::http::response<boost::beast::http::string_body>;

  LoginRouter(AuthService &auth, NativeAuth &native, NewsStore &news, PortalStore &content,
              std::string public_address, std::string service_key, std::string database_identity,
              std::shared_ptr<distribution::PatchCatalog> patches = {});

  std::shared_ptr<AuthAdmission::Permit> try_auth_work(std::string_view peer,
                                                       bool private_api = false);

  bool authenticated(const std::string &token, bool native_channel);

  std::string auth_frame(const std::string &token, std::string_view message);

  Response handle(const Request &request, bool peer_loopback, const std::string &peer_address);

  std::optional<boost::beast::http::response<boost::beast::http::file_body>>
  patch_blob(const Request &request) const;

private:
  Response discord_response(const Request &request, const std::string &target);

  AuthService &auth_;
  NativeAuth &native_;
  NewsStore &news_;
  PortalStore &content_;
  std::string public_address_;
  std::string service_key_;
  std::string database_identity_;
  std::shared_ptr<distribution::PatchCatalog> patches_;
  DiscordOAuth discord_;
  AuthReplayGuard replay_guard_;
  AuthAdmission public_admission_{16, 4, 12, 8192};
  AuthAdmission private_admission_{128, 128, 12, 8192};
};

// Both listeners share the authority but only the loopback listener accepts RPC.
// Password/database work executes on bounded workers, never on the socket executor.
void run_login_listeners(const LoginListenerOptions &options, LoginRouter &router);

} // namespace chronicles
