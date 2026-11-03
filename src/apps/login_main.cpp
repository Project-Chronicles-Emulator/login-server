#include "apps/postgres_server_lease.hpp"
#include "auth/auth.hpp"
#include "auth/auth_authority.hpp"
#include "auth/native_auth.hpp"
#include "distribution/distribution.hpp"
#include "persistence/character_repositories.hpp"
#include "persistence/memory_database.hpp"
#include "persistence/postgres_repositories.hpp"
#include "services/login/login_service.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <spdlog/spdlog.h>

namespace chronicles {
namespace {

struct Options {
  LoginListenerOptions listener;
  std::string public_address;
  std::string game_api_address{"http://127.0.0.1:8082"};
  std::string storage{"postgres"};
  std::string migration;
  std::string service_key_file;
  std::string patch_directory;
  std::string patch_public_key;
};

std::string environment_value(const char *name) {
  const auto value = std::getenv(name);
  return value ? value : "";
}

std::string read_file(const std::string &path, std::uintmax_t maximum_bytes) {
  if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > maximum_bytes) {
    throw std::invalid_argument("The configured file is missing or exceeds its size limit");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::invalid_argument("The configured file cannot be read");
  }
  return std::string(std::istreambuf_iterator<char>(input), {});
}

unsigned bounded_number(const std::string &text, unsigned minimum, unsigned maximum) {
  unsigned number{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number < minimum ||
      number > maximum) {
    throw std::invalid_argument("A numeric login option is outside its accepted range");
  }
  return number;
}

Options parse_options(int argc, char **argv) {
  std::map<std::string, std::string> values;
  const std::set<std::string> known{"port",
                                    "bind",
                                    "public-address",
                                    "game-api-address",
                                    "storage",
                                    "migrate",
                                    "authority-port",
                                    "auth-service-key-file",
                                    "http-tls-cert",
                                    "http-tls-key",
                                    "max-http-connections",
                                    "patch-dir",
                                    "patch-public-key"};
  // Configuration is loaded first; explicit command-line options override it.
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) != "--config") {
      continue;
    }
    if (index + 1 >= argc || values.contains("configuration-loaded")) {
      throw std::invalid_argument("Supply exactly one value for --config");
    }
    const auto document = nlohmann::json::parse(read_file(argv[++index], 1024 * 1024));
    if (!document.is_object()) {
      throw std::invalid_argument("Login configuration must be a JSON object");
    }
    for (const auto &[name, value] : document.items()) {
      auto option = name;
      std::replace(option.begin(), option.end(), '_', '-');
      if (!known.contains(option) || (!value.is_string() && !value.is_number_unsigned())) {
        throw std::invalid_argument("Unknown or invalid login configuration option");
      }
      values[option] = value.is_string() ? value.get<std::string>() : value.dump();
    }
    values["configuration-loaded"] = "true";
  }
  for (int index = 1; index < argc; ++index) {
    const std::string option(argv[index]);
    if (option == "--config") {
      ++index;
      continue;
    }
    if (!option.starts_with("--") || !known.contains(option.substr(2)) || index + 1 >= argc) {
      throw std::invalid_argument("Unknown login option or missing option value");
    }
    values[option.substr(2)] = argv[++index];
  }

  Options options;
  const auto text = [&](const char *name, const std::string &fallback = {}) {
    const auto found = values.find(name);
    return found == values.end() ? fallback : found->second;
  };
  options.listener.port =
      static_cast<unsigned short>(bounded_number(text("port", "8080"), 1, 65535));
  options.listener.authority_port =
      static_cast<unsigned short>(bounded_number(text("authority-port", "8090"), 1, 65535));
  options.listener.maximum_connections =
      bounded_number(text("max-http-connections", "1280"), 1, 100000);
  options.listener.bind_address = text("bind", "127.0.0.1");
  options.listener.certificate_file = text("http-tls-cert");
  options.listener.private_key_file = text("http-tls-key");
  options.storage = text("storage", "postgres");
  options.game_api_address =
      distribution::parse_service_origin(text("game-api-address", options.game_api_address)).url;
  options.service_key_file =
      text("auth-service-key-file", environment_value("CHRONICLES_AUTH_SERVICE_KEY_FILE"));
  options.migration = text("migrate");
  options.patch_directory = text("patch-dir");
  options.patch_public_key = text("patch-public-key");

  const bool tls = !options.listener.certificate_file.empty();
  if (tls != !options.listener.private_key_file.empty() ||
      options.listener.port == options.listener.authority_port ||
      options.service_key_file.empty()) {
    throw std::invalid_argument(
        "Login TLS, private port or service-key configuration is incomplete");
  }
  options.public_address =
      distribution::parse_service_origin(
          text("public-address", std::string(tls ? "https" : "http") +
                                     "://127.0.0.1:" + std::to_string(options.listener.port)))
          .url;
  const auto public_origin = distribution::parse_service_origin(options.public_address);
  const auto bind_address = boost::asio::ip::make_address(options.listener.bind_address);
  if (public_origin.secure != tls ||
      (!bind_address.is_loopback() && (!tls || public_origin.loopback))) {
    throw std::invalid_argument(
        "A remote login listener requires HTTPS and a remote public origin");
  }
  if (options.storage != "postgres" && options.storage != "memory") {
    throw std::invalid_argument("Login storage must be postgres or memory");
  }
  if (options.patch_directory.empty() != options.patch_public_key.empty()) {
    throw std::invalid_argument("Patch directory and public key must be supplied together");
  }
  return options;
}

void bootstrap_account(AuthService &auth) {
  const auto name = environment_value("CHRONICLES_BOOTSTRAP_ACCOUNT");
  if (name.empty()) {
    return;
  }
  const auto password = environment_value("CHRONICLES_BOOTSTRAP_PASSWORD");
  if (password.empty()) {
    throw std::invalid_argument("A configured bootstrap account requires its password");
  }
  auth.create_account(name, password);
  const auto role = environment_value("CHRONICLES_BOOTSTRAP_ACCOUNT_ROLE");
  if (!role.empty()) {
    const auto parsed = parse_account_role(role);
    if (!parsed || !auth.set_account_role(name, *parsed)) {
      throw std::invalid_argument("Bootstrap account role is invalid");
    }
  }
}

} // namespace
} // namespace chronicles

int main(int argc, char **argv) {
  using namespace chronicles;
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout
        << "Project Chronicles login server\n"
           "Options: [--config JSON] [--port PORT] [--bind IP] [--public-address ORIGIN]\n"
           "         [--game-api-address ORIGIN] [--authority-port PORT]\n"
           "         --auth-service-key-file FILE [--storage postgres|memory]\n"
           "         [--migrate SQL] [--http-tls-cert PEM --http-tls-key PEM]\n"
           "         [--max-http-connections COUNT] [--patch-dir DIR --patch-public-key HEX]\n"
           "Environment: CHRONICLES_DATABASE_URL, CHRONICLES_AUTH_SERVICE_KEY_FILE,\n"
           "             CHRONICLES_BOOTSTRAP_ACCOUNT, CHRONICLES_BOOTSTRAP_PASSWORD\n";
    return 0;
  }

  std::string phase = "configuration";
  try {
    const auto options = parse_options(argc, argv);
    auto service_key = read_auth_service_key(options.service_key_file);

    phase = "database ownership";
    const auto connection = environment_value("CHRONICLES_DATABASE_URL");
    std::unique_ptr<PostgresServerLease> database_lease;
    std::unique_ptr<HostServerLease> host_lease;
    std::string identity = "disposable-memory-login";
    if (options.storage == "postgres") {
      database_lease = std::make_unique<PostgresServerLease>(connection, ServerLeaseRole::login);
      identity = database_lease->database_identity();
      host_lease = std::make_unique<HostServerLease>(identity, ServerLeaseRole::login);
      if (!options.migration.empty()) {
        phase = "schema installation";
        migrate_postgres(connection, read_file(options.migration, 4 * 1024 * 1024));
      }
    }

    phase = "repository initialization";
    auto repositories =
        options.storage == "postgres"
            ? make_postgres_character_repositories(connection)
            : make_memory_character_repositories(std::make_shared<MemoryDatabase>());
    Config config;
    config.authority_database_identity = identity;
    config.lobby_address = options.game_api_address;
    AuthService auth(std::move(repositories), config);
    auth.require_world_heartbeat();
    phase = "account bootstrap";
    bootstrap_account(auth);

    NativeAuthConfig native_config;
    native_config.enabled = true;
    native_config.require_launch_grant = true;
    native_config.public_address = options.public_address;
    native_config.region = "local";
    native_config.client_version = "hotfix-26.10.18.734";
    native_config.client_protocol =
        "b0fa233f08edc242480aa097e76541d4e256bd7641c856c424907eb2ecd91e15";
    NativeAuth native(auth, native_config);
    NewsStore news(options.storage == "postgres" ? connection : std::string{});
    PortalStore content(options.storage == "postgres" ? connection : std::string{});
    std::shared_ptr<distribution::PatchCatalog> patches;
    if (!options.patch_directory.empty()) {
      patches = std::make_shared<distribution::PatchCatalog>(
          options.patch_directory, options.patch_public_key, options.public_address);
    }
    LoginRouter router(auth, native, news, content, options.public_address, std::move(service_key),
                       identity, std::move(patches));
    phase = "listener startup";
    run_login_listeners(options.listener, router);
    return 0;
  } catch (const std::exception &) {
    // Connection strings, account values and OAuth details are not startup diagnostics.
    spdlog::error("Login service failed during {}. Verify the configured inputs and permissions.",
                  phase);
    return 1;
  }
}
