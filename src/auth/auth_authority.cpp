#include "auth/auth_authority.hpp"

#include "auth/auth.hpp"
#include "auth/auth_wire.hpp"
#include "distribution/distribution.hpp"

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace chronicles {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using Tcp = asio::ip::tcp;

constexpr auto kAuthorityOperationTimeout = std::chrono::seconds(3);
constexpr std::string_view kAuthorityPrefix = "/internal/auth/v1/";

bool valid_operation(std::string_view operation) {
  return !operation.empty() && operation.size() <= 64 &&
         std::all_of(operation.begin(), operation.end(), [](char character) {
           return (character >= 'a' && character <= 'z') || character == '_';
         });
}

class LocalAuthAuthority final : public AuthAuthority {
public:
  LocalAuthAuthority(distribution::ServiceOrigin origin, std::string service_key,
                     std::string database_identity)
      : origin_(std::move(origin)), service_key_(std::move(service_key)),
        database_identity_(std::move(database_identity)) {}

  nlohmann::json call(std::string_view operation, const nlohmann::json &arguments) override {
    if (!valid_operation(operation) || !arguments.is_object()) {
      throw std::invalid_argument("Invalid authentication authority operation");
    }

    const auto sealed = seal_auth_request(service_key_, operation, database_identity_, arguments);
    const auto body = sealed.envelope.dump();

    // Each call has its own stream/context: workers do not share a socket, parser,
    // pending reply, or deadline. A lost reply is never automatically replayed.
    asio::io_context context;
    beast::tcp_stream stream(context);
    const auto host = origin_.host == "localhost" ? "127.0.0.1" : origin_.host;
    const Tcp::endpoint endpoint(asio::ip::make_address(host), origin_.port);
    boost::system::error_code error;

    stream.expires_after(kAuthorityOperationTimeout);
    stream.async_connect(endpoint, [&](boost::system::error_code result) {
      error = result;
    });
    context.run();
    if (error) {
      throw std::runtime_error("Authentication authority connection failed");
    }

    http::request<http::string_body> request(
        http::verb::post, std::string(kAuthorityPrefix) + std::string(operation), 11);
    request.set(http::field::host, origin_.host);
    request.set(http::field::authorization, std::string(kAuthWireAuthorization));
    if (!database_identity_.empty()) {
      request.set("X-Chronicles-Database", database_identity_);
    }
    request.set(http::field::content_type, "application/json");
    request.keep_alive(false);
    request.body() = body;
    request.prepare_payload();

    context.restart();
    stream.expires_after(kAuthorityOperationTimeout);
    http::async_write(stream, request, [&](boost::system::error_code result, std::size_t) {
      error = result;
    });
    context.run();
    if (error) {
      throw std::runtime_error("Authentication authority request failed");
    }

    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(kMaximumAuthWireBytes);
    parser.header_limit(16 * 1024);
    context.restart();
    stream.expires_after(kAuthorityOperationTimeout);
    http::async_read(stream, buffer, parser, [&](boost::system::error_code result, std::size_t) {
      error = result;
    });
    context.run();
    if (error) {
      throw std::runtime_error("Authentication authority reply failed");
    }

    const auto &response = parser.get();
    const auto envelope = nlohmann::json::parse(response.body(), nullptr, false);
    const auto result =
        open_auth_reply(service_key_, sealed.context, response.result_int(), envelope);
    if (response.result() == http::status::unauthorized) {
      throw Unauthorized();
    }
    if (response.result() == http::status::bad_request) {
      throw std::invalid_argument("Authentication authority rejected invalid input");
    }
    if (response.result() != http::status::ok) {
      throw std::runtime_error("Authentication authority refused the operation");
    }

    return result;
  }

private:
  distribution::ServiceOrigin origin_;
  std::string service_key_;
  std::string database_identity_;
};

} // namespace

bool valid_auth_service_key(std::string_view key) {
  return key.size() == 64 && std::all_of(key.begin(), key.end(), [](char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

std::string read_auth_service_key(const std::string &path) {
  if (path.empty() || !std::filesystem::is_regular_file(path) ||
      std::filesystem::file_size(path) > 256) {
    throw std::invalid_argument(
        "The private authentication service key file is missing or invalid");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::invalid_argument("The private authentication service key file cannot be read");
  }
  std::string key(std::istreambuf_iterator<char>(input), {});
  while (!key.empty() && (key.back() == '\n' || key.back() == '\r')) {
    key.pop_back();
  }
  if (!valid_auth_service_key(key)) {
    throw std::invalid_argument(
        "The private authentication service key must contain 64 hex digits");
  }
  return key;
}

std::shared_ptr<AuthAuthority> make_local_auth_authority(const std::string &origin,
                                                         const std::string &service_key,
                                                         const std::string &database_identity) {
  const auto parsed = distribution::parse_service_origin(origin);
  if (!parsed.loopback || parsed.secure || !valid_auth_service_key(service_key) ||
      (!database_identity.empty() && !valid_auth_service_key(database_identity))) {
    throw std::invalid_argument(
        "The private authentication authority requires loopback HTTP and a 32-byte key");
  }
  return std::make_shared<LocalAuthAuthority>(parsed, service_key, database_identity);
}

} // namespace chronicles
