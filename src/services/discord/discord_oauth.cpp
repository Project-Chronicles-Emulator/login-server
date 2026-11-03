#include "services/discord/discord_oauth.hpp"
#include "distribution/distribution.hpp"

#include <algorithm>
#include <cstdlib>
#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace chronicles {

nlohmann::json request_discord_https(const std::string &method, const std::string &path,
                                     const std::string &form, const std::string &bearer);

namespace {

using Json = nlohmann::json;
constexpr auto kAttemptLifetime = std::chrono::minutes(5);
constexpr std::size_t kMaximumAttempts = 256;
constexpr std::string_view kCallbackPath = "/emulator/v1/discord/callback";

bool snowflake(std::string_view value) {
  return value.size() >= 17 && value.size() <= 20 && value.front() != '0' &&
         std::all_of(value.begin(), value.end(),
                     [](char digit) { return digit >= '0' && digit <= '9'; });
}

std::string pkce_challenge(const std::string &verifier) {
  const auto digest = hex_decode(distribution::sha256_bytes(verifier));
  std::string encoded(4 * ((digest.size() + 2) / 3) + 1, '\0');
  const auto length = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                                      reinterpret_cast<const unsigned char *>(digest.data()),
                                      static_cast<int>(digest.size()));
  if (length <= 0) {
    throw std::runtime_error("Discord PKCE encoding failed");
  }
  encoded.resize(static_cast<std::size_t>(length));
  while (encoded.ends_with('=')) {
    encoded.pop_back();
  }
  std::replace(encoded.begin(), encoded.end(), '+', '-');
  std::replace(encoded.begin(), encoded.end(), '/', '_');
  return encoded;
}

int hex_digit(char digit) {
  if (digit >= '0' && digit <= '9') {
    return digit - '0';
  }
  if (digit >= 'a' && digit <= 'f') {
    return digit - 'a' + 10;
  }
  if (digit >= 'A' && digit <= 'F') {
    return digit - 'A' + 10;
  }
  throw std::invalid_argument("Malformed OAuth query");
}

std::string decode_query(std::string_view value) {
  std::string result;
  for (std::size_t index = 0; index < value.size(); ++index) {
    char character = value[index];
    if (character == '%') {
      if (index + 2 >= value.size()) {
        throw std::invalid_argument("Malformed OAuth query");
      }
      character =
          static_cast<char>((hex_digit(value[index + 1]) << 4) | hex_digit(value[index + 2]));
      index += 2;
    } else if (character == '+') {
      character = ' ';
    }
    if (static_cast<unsigned char>(character) < 32 || character == 127) {
      throw std::invalid_argument("Invalid OAuth query character");
    }
    result.push_back(character);
  }
  return result;
}

} // namespace

std::string oauth_form_encode(std::string_view value) {
  constexpr char hexadecimal[] = "0123456789ABCDEF";
  std::string result;
  for (const unsigned char character : value) {
    if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' || character == '_' ||
        character == '.' || character == '~') {
      result.push_back(static_cast<char>(character));
    } else {
      result.push_back('%');
      result.push_back(hexadecimal[character >> 4]);
      result.push_back(hexadecimal[character & 15]);
    }
  }
  return result;
}

std::unordered_map<std::string, std::string> oauth_query(std::string_view query) {
  if (query.size() > 8192) {
    throw std::invalid_argument("OAuth query is too large");
  }
  std::unordered_map<std::string, std::string> result;
  while (!query.empty()) {
    const auto separator = query.find('&');
    const auto item = query.substr(0, separator);
    const auto equals = item.find('=');
    const auto key = decode_query(item.substr(0, equals));
    const auto value =
        equals == std::string_view::npos ? std::string{} : decode_query(item.substr(equals + 1));
    if (key.empty() || !result.emplace(key, value).second || result.size() > 16) {
      throw std::invalid_argument("Duplicate or invalid OAuth query parameter");
    }
    if (separator == std::string_view::npos) {
      break;
    }
    query.remove_prefix(separator + 1);
  }
  return result;
}

bool DiscordOAuthConfig::enabled() const {
  return !client_id.empty() && !client_secret.empty() && !redirect_uri.empty();
}

DiscordOAuthConfig DiscordOAuthConfig::parse(std::string_view text) {
  try {
    const auto document = Json::parse(text);
    if (!document.value("enabled", false)) {
      return {};
    }
    return {document.at("client_id").get<std::string>(),
            document.at("client_secret").get<std::string>(),
            document.at("redirect_uri").get<std::string>()};
  } catch (const std::exception &) {
    // JSON parse diagnostics can include source text containing the app secret.
    throw std::runtime_error(
        "Cannot read Discord OAuth configuration. Check its private file format.");
  }
}

DiscordOAuthConfig DiscordOAuthConfig::from_environment() {
  const auto *path = std::getenv("CHRONICLES_DISCORD_OAUTH_CONFIG");
  if (path == nullptr || *path == '\0') {
    return {};
  }
  try {
    return parse(distribution::read_bounded_file(path, 16384));
  } catch (const std::exception &) {
    throw std::runtime_error(
        "Cannot read Discord OAuth configuration. Check its private file format.");
  }
}

DiscordOAuth::DiscordOAuth(AuthService &auth, DiscordOAuthConfig config, DiscordRequest request,
                           std::function<Clock::time_point()> now)
    : auth_(auth), config_(std::move(config)),
      request_(request ? std::move(request) : request_discord_https), now_(std::move(now)) {
  if (!config_.enabled()) {
    return;
  }
  if (!snowflake(config_.client_id) || config_.client_secret.size() > 512 ||
      config_.redirect_uri.size() > 2048 ||
      config_.client_secret.find("REPLACE") != std::string::npos ||
      !config_.redirect_uri.ends_with(kCallbackPath)) {
    throw std::invalid_argument("Invalid Discord OAuth server configuration");
  }
  distribution::parse_service_origin(
      config_.redirect_uri.substr(0, config_.redirect_uri.size() - kCallbackPath.size()));
}

bool DiscordOAuth::enabled() const {
  return config_.enabled();
}

void DiscordOAuth::prune() {
  const auto now = now_();
  std::erase_if(attempts_, [now](const auto &entry) { return entry.second.expires <= now; });
}

Json DiscordOAuth::begin(unsigned short loopback_port, const std::string &link_session) {
  if (!enabled()) {
    throw std::runtime_error("Discord sign-in is not configured on this server");
  }
  if (!link_session.empty() && !auth_.authenticated(link_session)) {
    throw Unauthorized();
  }
  if (loopback_port < 1024) {
    throw std::invalid_argument("Invalid Discord loopback callback port");
  }
  Attempt attempt;
  attempt.poll_secret = random_hex(32);
  attempt.verifier = random_hex(32);
  attempt.browser_proof = random_hex(32);
  attempt.loopback_port = loopback_port;
  attempt.link_session = link_session;
  attempt.expires = now_() + kAttemptLifetime;
  const auto state = random_hex(32);
  const auto url =
      "https://discord.com/oauth2/authorize?response_type=code&scope=identify&client_id=" +
      config_.client_id + "&redirect_uri=" + oauth_form_encode(config_.redirect_uri) +
      "&state=" + state +
      "&code_challenge_method=S256&code_challenge=" + pkce_challenge(attempt.verifier);
  Json response{{"authorization_url", url},
                {"request_id", state},
                {"poll_secret", attempt.poll_secret},
                {"expires_in", 300}};
  std::lock_guard lock(mutex_);
  prune();
  if (attempts_.size() >= kMaximumAttempts) {
    throw std::runtime_error("Too many Discord sign-in attempts. Try again shortly.");
  }
  attempts_.emplace(state, std::move(attempt));
  return response;
}

std::string DiscordOAuth::callback(const std::string &state, const std::string &code,
                                   const std::string &issuer, bool denied) {
  Attempt attempt;
  {
    std::lock_guard lock(mutex_);
    prune();
    const auto found = attempts_.find(state);
    if (found == attempts_.end() || found->second.callback_started) {
      throw std::invalid_argument("Discord sign-in expired or was already completed");
    }
    found->second.callback_started = true;
    attempt = found->second;
  }
  Json result{{"status", "failed"},
              {"error", "Discord sign-in could not complete. Please try again."}};
  std::string verified_subject;
  try {
    if (denied || issuer != "https://discord.com" || code.empty() || code.size() > 2048) {
      throw std::invalid_argument("Discord authorization was denied or invalid");
    }
    const auto form = "grant_type=authorization_code&client_id=" + config_.client_id +
                      "&client_secret=" + oauth_form_encode(config_.client_secret) +
                      "&redirect_uri=" + oauth_form_encode(config_.redirect_uri) +
                      "&code=" + oauth_form_encode(code) +
                      "&code_verifier=" + oauth_form_encode(attempt.verifier);
    const auto token = request_("POST", "/api/oauth2/token", form, {});
    const auto access_token = token.at("access_token").get<std::string>();
    if (access_token.empty() || access_token.size() > 2048 ||
        token.value("token_type", std::string{}) != "Bearer") {
      throw std::runtime_error("Invalid Discord token response");
    }
    const auto info = request_("GET", "/api/v10/oauth2/@me", {}, access_token);
    const auto scopes = info.at("scopes").get<std::vector<std::string>>();
    const auto &user = info.at("user");
    const auto subject = user.at("id").get<std::string>();
    if (info.at("application").at("id") != config_.client_id || !snowflake(subject) ||
        user.value("bot", false) || std::ranges::find(scopes, "identify") == scopes.end() ||
        now_() >= attempt.expires) {
      throw std::runtime_error("Discord application or identity did not match");
    }
    verified_subject = subject;
    result = {{"status", "authorized"}};
  } catch (const std::exception &) {
    // Never relay provider bodies, codes, credentials or user metadata in errors.
  }
  std::lock_guard lock(mutex_);
  const auto found = attempts_.find(state);
  if (found != attempts_.end()) {
    found->second.result = std::move(result);
    found->second.verifier.clear();
    found->second.verified_discord_id = std::move(verified_subject);
  }
  // Only the browser that completed authorization receives this proof. A remote
  // requester holding a polling secret cannot collect another browser's login.
  return "http://127.0.0.1:" + std::to_string(attempt.loopback_port) +
         "/discord-complete?state=" + state + "&proof=" + attempt.browser_proof;
}

Json DiscordOAuth::poll(const std::string &request_id, const std::string &poll_secret,
                        const std::string &browser_proof) {
  Attempt completed;
  {
    std::lock_guard lock(mutex_);
    prune();
    const auto found = attempts_.find(request_id);
    if (found == attempts_.end() || poll_secret.size() != found->second.poll_secret.size() ||
        CRYPTO_memcmp(poll_secret.data(), found->second.poll_secret.data(), poll_secret.size()) !=
            0) {
      throw Unauthorized();
    }
    if (browser_proof.empty()) {
      return {{"status", "pending"}};
    }
    if (browser_proof.size() != found->second.browser_proof.size() ||
        CRYPTO_memcmp(browser_proof.data(), found->second.browser_proof.data(),
                      browser_proof.size()) != 0) {
      throw Unauthorized();
    }
    if (found->second.result.at("status") == "pending") {
      return {{"status", "pending"}};
    }
    completed = std::move(found->second);
    attempts_.erase(found);
  }
  if (completed.result.at("status") != "authorized") {
    return completed.result;
  }

  // The browser proof must reach the initiating PC before account ownership or
  // sessions change. Consume the attempt before database work, outside the lock.
  try {
    if (!completed.link_session.empty()) {
      if (auth_.link_discord(completed.link_session, completed.verified_discord_id)) {
        return {{"status", "linked"}};
      }
    } else {
      const auto login = auth_.login_discord(completed.verified_discord_id);
      if (login.ec() == x5::ec::Success) {
        return {{"status", "signed_in"},
                {"access_token", login.access_token()},
                {"account", auth_.account_name(login.access_token())}};
      }
    }
  } catch (const std::exception &) {
    // Provider and account details must not escape through public responses.
  }
  return {{"status", "failed"}, {"error", "Discord sign-in could not complete. Please try again."}};
}

} // namespace chronicles
