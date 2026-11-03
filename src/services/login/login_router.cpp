#include "services/login/login_service.hpp"

#include "auth/auth_authority.hpp"
#include "auth/password_policy.hpp"
#include "net/native_ws_frames.hpp"

#include <algorithm>
#include <cctype>

namespace chronicles {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
using Request = LoginRouter::Request;
using Response = LoginRouter::Response;
using Json = nlohmann::json;
constexpr std::string_view kPrivatePrefix = "/internal/auth/v1/";

Response json_response(http::status status, const Json &body) {
  Response response(status, 11);
  response.set(http::field::content_type, "application/json");
  response.set(http::field::cache_control, "no-store");
  response.set("X-Content-Type-Options", "nosniff");
  if (status != http::status::no_content) {
    response.body() = body.dump();
  }
  response.prepare_payload();
  return response;
}

Response error_response(http::status status, std::string_view error) {
  return json_response(status, {{"error", error}});
}

std::string bearer_token(const Request &request) {
  const auto authorization = request[http::field::authorization];
  if (!authorization.starts_with("Bearer ") || authorization.size() > 4103) {
    return {};
  }
  return std::string(authorization.substr(7));
}

template <typename Message>
Response protobuf_response(const Request &request, const Message &message) {
  if (request[http::field::accept] != "application/x-protobuf") {
    return json_response(http::status::ok, message_json(message));
  }
  Response response(http::status::ok, 11);
  response.set(http::field::content_type, "application/x-protobuf");
  response.set(http::field::cache_control, "no-store");
  response.body() = message.SerializeAsString();
  response.prepare_payload();
  return response;
}

std::string normalized_account(std::string account) {
  if (account.size() > 32) {
    return {};
  }
  std::transform(account.begin(), account.end(), account.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return account;
}

} // namespace

LoginRouter::LoginRouter(AuthService &auth, NativeAuth &native, NewsStore &news,
                         PortalStore &content, std::string public_address, std::string service_key,
                         std::string database_identity,
                         std::shared_ptr<distribution::PatchCatalog> patches)
    : auth_(auth), native_(native), news_(news), content_(content),
      public_address_(std::move(public_address)), service_key_(std::move(service_key)),
      database_identity_(std::move(database_identity)), patches_(std::move(patches)),
      discord_(auth, DiscordOAuthConfig::from_environment()) {
  if (!valid_auth_service_key(service_key_) || database_identity_.empty()) {
    throw std::invalid_argument("Login service identity or private service key is invalid");
  }
}

std::shared_ptr<AuthAdmission::Permit> LoginRouter::try_auth_work(std::string_view peer,
                                                                  bool private_api) {
  return private_api ? private_admission_.try_acquire(peer) : public_admission_.try_acquire(peer);
}

bool LoginRouter::authenticated(const std::string &token, bool native_channel) {
  return native_channel ? native_.authenticate_session(token) : auth_.authenticated(token);
}

std::string LoginRouter::auth_frame(const std::string &token, std::string_view message) {
  if (!auth_.authenticated(token)) {
    throw Unauthorized();
  }
  std::string output;
  for (const auto &frame : native_ws::decode_frames(message)) {
    if (frame.packet_id != native_ws::kWorldEnterRequest) {
      throw std::invalid_argument("Unknown login WebSocket request");
    }
    const auto request =
        native_ws::parse_payload<x5::pb::authwebsocket::WorldEnterReq>(frame.payload);
    output += native_ws::encode_frame(native_ws::kWorldEnterResponse, auth_.enter(token, request));
  }
  return output;
}

std::optional<http::response<http::file_body>>
LoginRouter::patch_blob(const Request &request) const {
  const std::string_view target(request.target().data(), request.target().size());
  if (!patches_ || request.method() != http::verb::get ||
      !target.starts_with(distribution::kBlobRoute)) {
    return std::nullopt;
  }
  try {
    const auto path = patches_->blob(std::string(target.substr(distribution::kBlobRoute.size())));
    http::response<http::file_body> response(http::status::ok, 11);
    beast::error_code error;
    response.body().open(path.string().c_str(), beast::file_mode::scan, error);
    if (error) {
      return std::nullopt;
    }
    response.set(http::field::content_type, "application/octet-stream");
    response.set(http::field::cache_control, "public, max-age=31536000, immutable");
    response.set("X-Content-Type-Options", "nosniff");
    response.content_length(response.body().size());
    return response;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

LoginRouter::Response LoginRouter::handle(const Request &request, bool peer_loopback,
                                          const std::string &peer_address) {
  try {
    const std::string target(request.target().data(), request.target().size());
    const auto path = target.substr(0, target.find('?'));
    if (path.starts_with(kPrivatePrefix)) {
      if (!peer_loopback || request[http::field::authorization] != kAuthWireAuthorization ||
          request["X-Chronicles-Database"] != database_identity_) {
        return error_response(http::status::unauthorized,
                              "private_service_authentication_required");
      }
      if (request.method() != http::verb::post ||
          !request[http::field::content_type].starts_with("application/json")) {
        return error_response(http::status::bad_request, "invalid_authority_request");
      }
      const auto opened = open_auth_request(service_key_, path.substr(kPrivatePrefix.size()),
                                            database_identity_, Json::parse(request.body()));
      auto status = http::status::ok;
      Json reply;
      try {
        if (!replay_guard_.accept(opened.context.request_id)) {
          throw Unauthorized();
        }
        reply = auth_.handle_authority_operation(opened.context.operation, opened.arguments);
      } catch (const Unauthorized &) {
        status = http::status::unauthorized;
        reply = {{"error", "unauthorized"}};
      } catch (const std::invalid_argument &) {
        status = http::status::bad_request;
        reply = {{"error", "invalid_authority_request"}};
      } catch (const Json::exception &) {
        status = http::status::bad_request;
        reply = {{"error", "invalid_authority_request"}};
      } catch (const std::exception &) {
        status = http::status::service_unavailable;
        reply = {{"error", "authority_unavailable"}};
      }
      return json_response(status, seal_auth_reply(service_key_, opened.context,
                                                   static_cast<unsigned>(status), reply));
    }
    if (path == "/health" && request.method() == http::verb::get) {
      const auto world = auth_.world_status();
      return json_response(http::status::ok,
                           {{"status", "ok"},
                            {"service", "login"},
                            {"native_login_adapter", true},
                            {"native_require_launch_grant", true},
                            {"native_websockets", true},
                            {"world_listener", false},
                            {"patch_service", patches_ != nullptr},
                            {"world",
                             {{"id", world.world_id},
                              {"name", world.name},
                              {"online", world.online},
                              {"capacity", world.capacity},
                              {"state", world_state_label(world.state)},
                              {"char_creation_restricted", world.char_creation_restricted}}}});
    }
    if (path == "/emulator/v1/auth-options" || path.starts_with("/emulator/v1/discord/")) {
      return discord_response(request, target);
    }
    if (path == "/launcher/v1/news" && request.method() == http::verb::get) {
      return json_response(http::status::ok, {{"articles", news_.list()}});
    }
    if (path == "/launcher/v1/content" && request.method() == http::verb::get) {
      return json_response(http::status::ok, {{"store", content_.list("store")},
                                              {"community", content_.list("community")}});
    }
    constexpr std::string_view channel_prefix = "/distribution/v1/channels/";
    if (patches_ && request.method() == http::verb::get && path.starts_with(channel_prefix)) {
      auto response = json_response(http::status::ok, Json::object());
      response.body() = patches_->manifest(path.substr(channel_prefix.size()));
      response.prepare_payload();
      return response;
    }

    const auto native =
        native_.handle(std::string_view(request.method_string()), target,
                       std::string_view(request[http::field::authorization]),
                       std::string_view(request[http::field::content_type]), request.body());
    if (native) {
      Response response(static_cast<http::status>(native->status), 11);
      response.set(http::field::content_type, native->content_type);
      response.set(http::field::cache_control, "no-store");
      response.body() = native->body;
      response.prepare_payload();
      return response;
    }
    if (path == "/emulator/v1/register" && request.method() == http::verb::post) {
      if (request.body().size() > 4096 || !public_admission_.allow_registration(peer_address)) {
        return error_response(http::status::too_many_requests, "registration_rate_limit");
      }
      const auto document = Json::parse(request.body());
      const auto created = auth_.create_account(document.at("account").get<std::string>(),
                                                document.at("password").get<std::string>());
      return created ? json_response(http::status::created, {{"created", true}})
                     : error_response(http::status::conflict, "account_unavailable");
    }
    if (path == "/emulator/v1/login" && request.method() == http::verb::post) {
      const auto document = Json::parse(request.body());
      const auto name = document.at("account").get<std::string>();
      const auto normalized = normalized_account(name);
      if (!public_admission_.password_allowed(peer_address, normalized)) {
        auto response = error_response(http::status::too_many_requests, "login_rate_limit");
        response.set(http::field::retry_after, "60");
        return response;
      }
      bool mismatch{};
      const auto result =
          auth_.login(name, document.at("password").get<std::string>(),
                      document.value("client", std::string{}) == "launcher", &mismatch);
      if (mismatch && !normalized.empty()) {
        public_admission_.bad_password(peer_address, normalized);
      }
      return protobuf_response(request, result);
    }

    const auto token = bearer_token(request);
    if (!auth_.authenticated(token)) {
      return error_response(http::status::unauthorized, "authentication_required");
    }
    if (path == "/launcher/v1/characters" && request.method() == http::verb::get) {
      return json_response(http::status::ok, auth_.launcher_characters(token));
    }
    if (path == "/emulator/v1/launch-grant" && request.method() == http::verb::post) {
      const auto grant = auth_.issue_launch_grant(token);
      if (!grant) {
        return error_response(http::status::unauthorized, "authentication_required");
      }
      return json_response(http::status::ok,
                           {{"bootstrap_url", public_address_ + "/launch/" + *grant + "/"},
                            {"expires_in_seconds", kLaunchGrantTtl.count()}});
    }
    if (path == "/emulator/v1/logout" && request.method() == http::verb::post) {
      auth_.logout(token);
      return json_response(http::status::no_content, Json::object());
    }
    if (path == "/emulator/v1/worlds" && request.method() == http::verb::get) {
      return protobuf_response(request, auth_.worlds(token));
    }
    return error_response(http::status::not_found, "route_not_found");
  } catch (const Unauthorized &) {
    return error_response(http::status::unauthorized, "authentication_required");
  } catch (const Json::exception &) {
    return error_response(http::status::bad_request, "invalid_request");
  } catch (const std::invalid_argument &) {
    return error_response(http::status::bad_request, "invalid_request");
  } catch (const std::exception &) {
    // Database and OAuth error text can contain private connection or session values.
    return error_response(http::status::service_unavailable, "login_service_unavailable");
  }
}

LoginRouter::Response LoginRouter::discord_response(const Request &request,
                                                    const std::string &target) {
  const auto separator = target.find('?');
  const auto path = target.substr(0, separator);
  if (path == "/emulator/v1/auth-options" && request.method() == http::verb::get) {
    return json_response(http::status::ok, {{"discord", discord_.enabled()},
                                            {"minimum_password_bytes", kMinimumPasswordBytes}});
  }
  if (!discord_.enabled()) {
    return error_response(http::status::service_unavailable, "discord_not_configured");
  }
  if (path == "/emulator/v1/discord/callback" && request.method() == http::verb::get) {
    const auto query = oauth_query(separator == std::string::npos
                                       ? std::string_view{}
                                       : std::string_view(target).substr(separator + 1));
    if (!query.contains("state")) {
      return error_response(http::status::bad_request, "invalid_discord_callback");
    }
    const auto destination =
        discord_.callback(query.at("state"), query.contains("code") ? query.at("code") : "",
                          query.contains("iss") ? query.at("iss") : "", query.contains("error"));
    Response response(http::status::see_other, 11);
    response.set(http::field::location, destination);
    response.set(http::field::cache_control, "no-store");
    response.set("Referrer-Policy", "no-referrer");
    response.prepare_payload();
    return response;
  }
  if (request.method() != http::verb::post || request.body().size() > 2048 ||
      !request[http::field::content_type].starts_with("application/json")) {
    return error_response(http::status::bad_request, "invalid_discord_request");
  }
  const auto document = Json::parse(request.body());
  if (path == "/emulator/v1/discord/start") {
    const auto port = document.at("loopback_port").get<std::int64_t>();
    const auto token = document.value("link", false) ? bearer_token(request) : std::string{};
    if (port < 1024 || port > 65535 || (document.value("link", false) && token.empty())) {
      return error_response(http::status::bad_request, "invalid_discord_callback");
    }
    return json_response(http::status::ok,
                         discord_.begin(static_cast<unsigned short>(port), token));
  }
  if (path == "/emulator/v1/discord/poll") {
    return json_response(http::status::ok,
                         discord_.poll(document.at("request_id").get<std::string>(),
                                       document.at("poll_secret").get<std::string>(),
                                       document.value("browser_proof", std::string{})));
  }
  return error_response(http::status::method_not_allowed, "invalid_discord_method");
}

} // namespace chronicles
