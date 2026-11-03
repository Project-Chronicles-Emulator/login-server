#include "auth/auth.hpp"
#include "auth/auth_authority.hpp"
#include "auth/password_policy.hpp"
#include "data/client_catalog.hpp"
#include "data/starter_gear.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

#include <google/protobuf/util/json_util.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace chronicles {
namespace {

constexpr std::size_t kMinimumAccountNameLength = 3;
constexpr std::size_t kMaximumAccountNameLength = 32;
constexpr std::size_t kPasswordSaltBytes = 16;
constexpr int kPasswordHashBytes = 32;
constexpr std::size_t kTokenBytes = 32;
constexpr std::size_t kPacketKeyBytes = 32;
constexpr std::size_t kMinimumDiagnosticMatchBytes = 4;
constexpr int kMinimumPasswordIterations = 100000;
constexpr int kMaximumPasswordIterations = 2000000;
constexpr std::size_t kMaximumSessions = 10000;
constexpr std::size_t kMaximumLaunchGrants = 10000;
constexpr std::size_t kMaximumSessionTokenBytes = 4096;
constexpr int kCharacterSlotCount = 4;
constexpr std::size_t kMinimumCharacterNameLength = 3;
constexpr std::size_t kMaximumCharacterNameLength = 16;

std::string reconnect_credential_digest(std::string_view credential) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size{};
  if (EVP_Digest(credential.data(), credential.size(), digest.data(), &digest_size,
                 EVP_sha256(), nullptr) != 1 || digest_size != 32) {
    throw std::runtime_error("Cannot hash reconnect credential");
  }

  return hex_encode(std::string(reinterpret_cast<const char *>(digest.data()), digest_size));
}

std::int64_t unix_time_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool is_ascii_alphanumeric(char character) {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9');
}

bool is_valid_account_name(const std::string &name) {
  return name.size() >= kMinimumAccountNameLength && name.size() <= kMaximumAccountNameLength &&
         std::all_of(name.begin(), name.end(), [](char character) {
           return is_ascii_alphanumeric(character) || character == '_';
         });
}

CredentialPrefixMatch longest_credential_prefix_match(std::string_view received,
                                                       std::string_view expected) {
  CredentialPrefixMatch best_match;
  for (std::size_t offset = 0; offset < received.size(); ++offset) {
    std::size_t length{};
    while (length < expected.size() && offset + length < received.size() &&
           received[offset + length] == expected[length]) {
      ++length;
    }

    if (length >= kMinimumDiagnosticMatchBytes && length > best_match.length) {
      best_match = CredentialPrefixMatch{offset, length};
    }
  }

  return best_match;
}

std::string derive_password_hash(const std::string &password, const std::string &salt,
                                 int iterations) {
  std::string hash(kPasswordHashBytes, '\0');
  if (iterations < kMinimumPasswordIterations || iterations > kMaximumPasswordIterations ||
      !PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                         reinterpret_cast<const unsigned char *>(salt.data()),
                         static_cast<int>(salt.size()), iterations, EVP_sha256(),
                         kPasswordHashBytes, reinterpret_cast<unsigned char *>(hash.data()))) {
    throw std::runtime_error("Password derivation failed");
  }

  return hex_encode(hash);
}

x5::pb::LobbyCharacter make_lobby_character(const Character &character) {
  x5::pb::LobbyCharacter lobby_character;
  lobby_character.set_char_id(character.id);
  lobby_character.set_name(character.name);
  lobby_character.set_lobby_slot(character.slot);
  lobby_character.set_char_class(static_cast<x5::cms::EX5CharacterClass>(character.class_id));
  lobby_character.set_level(character.level);
  lobby_character.set_world_level(1);
  if (!character.items.empty()) {
    std::array<int, kEquipSlotCount> equipped_ids{};
    for (const auto &item : character.items) {
      if (!item.equip_slot) {
        continue;
      }
      if (*item.equip_slot < 0 || *item.equip_slot >= kEquipSlotCount) {
        throw std::runtime_error("Stored equipment slot is invalid");
      }

      equipped_ids.at(*item.equip_slot) = item.cms_id;
    }

    for (const auto cms_id : equipped_ids) {
      lobby_character.add_equip_cms_ids(cms_id);
    }
    if (equipped_ids.at(character.build.active_weapon_slot) != 0) {
      lobby_character.set_weapon_slot(
          static_cast<x5::cms::EX5EquipSlotType>(character.build.active_weapon_slot));
    }
  }

  if (!character.customization_hex.empty() &&
      !lobby_character.mutable_customization()->ParseFromString(
          hex_decode(character.customization_hex))) {
    throw std::runtime_error("Stored customization is invalid");
  }
  return lobby_character;
}
} // namespace

std::string hex_encode(const std::string &bytes) {
  std::string result;
  result.reserve(bytes.size() * 2);
  for (unsigned char character : bytes) {
    result.push_back("0123456789abcdef"[character >> 4]);
    result.push_back("0123456789abcdef"[character & 15]);
  }

  return result;
}

std::string hex_decode(const std::string &hex) {
  if (hex.size() % 2) {
    throw std::runtime_error("Invalid hex size");
  }
  std::string result;

  auto hex_digit_value = [](char character) {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    throw std::runtime_error("Invalid hex");
  };

  for (std::size_t i = 0; i < hex.size(); i += 2) {
    result.push_back(
        static_cast<char>((hex_digit_value(hex[i]) << 4) | hex_digit_value(hex[i + 1])));
  }

  return result;
}

std::string random_hex(std::size_t byte_count) {
  std::string bytes(byte_count, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), static_cast<int>(byte_count)) !=
      1) {
    throw std::runtime_error("Random generation failed");
  }
  return hex_encode(bytes);
}

nlohmann::json message_json(const google::protobuf::Message &message) {
  std::string text;
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  auto status = google::protobuf::util::MessageToJsonString(message, &text, options);
  if (!status.ok()) {
    throw std::runtime_error(status.ToString());
  }
  return nlohmann::json::parse(text);
}

void parse_json(const nlohmann::json &json, google::protobuf::Message &message) {
  auto status = google::protobuf::util::JsonStringToMessage(json.dump(), &message);
  if (!status.ok()) {
    throw std::invalid_argument("Invalid protobuf JSON payload");
  }
}

AuthService::AuthService(CharacterRepositories repositories, Config config,
                         std::function<Clock::time_point()> now)
    : accounts_(std::move(repositories.accounts)),
      characters_(std::move(repositories.characters)),
      character_state_(std::move(repositories.character_state)),
      config_(std::move(config)), now_(std::move(now)) {
  if (!accounts_ || !characters_ || !character_state_) {
    throw std::invalid_argument(
        "AuthService requires account, character and character-state repositories");
  }
  realm_.world_name = config_.world_name;
  realm_.site_display_name = config_.site_display_name;
  realm_.capacity = config_.world_capacity;
  realm_.label = config_.world_label;
  realm_.maintenance = config_.world_maintenance;
  realm_.char_creation_restricted = config_.char_creation_restricted;
}

void validate_realm_text(std::string_view field, const std::string &value) {
  const auto invalid = [&] {
    return std::invalid_argument(std::string(field) + " must be 1-" +
                                 std::to_string(kMaximumRealmTextBytes) +
                                 " bytes without control characters");
  };
  if (value.empty() || value.size() > kMaximumRealmTextBytes) {
    throw invalid();
  }

  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7F) {
      throw invalid();
    }
  }
}

void validate_realm_settings(const RealmSettings &settings) {
  validate_realm_text("world_name", settings.world_name);
  validate_realm_text("site_display_name", settings.site_display_name);
  if (settings.capacity == 0 || settings.capacity > kMaximumWorldCapacity) {
    throw std::invalid_argument("capacity must be between 1 and " +
                                std::to_string(kMaximumWorldCapacity));
  }
  if (!x5::pb::ServerLabel_IsValid(settings.label)) {
    throw std::invalid_argument("label is not a recovered ServerLabel value");
  }
}

std::string_view world_state_label(x5::pb::ServerState state) {
  switch (state) {
  case x5::pb::ServerState_OffLine:
    return "offline";
  case x5::pb::ServerState_GmOnly:
    return "maintenance";
  case x5::pb::ServerState_Congested:
    return "congested";
  case x5::pb::ServerState_Overloaded:
    return "full";
  default:
    return "normal";
  }
}

bool is_valid_character_name(std::string_view name) {
  return name.size() >= kMinimumCharacterNameLength &&
         name.size() <= kMaximumCharacterNameLength &&
         std::all_of(name.begin(), name.end(),
                     [](char character) { return is_ascii_alphanumeric(character); });
}

RealmSettings AuthService::realm_settings() const {
  std::lock_guard lock(realm_mutex_);
  return realm_;
}

void AuthService::update_realm_settings(const RealmSettings &settings) {
  validate_realm_settings(settings);
  if (authority_) {
    const auto result = authority_->call(
        "set_realm", {{"world_name", settings.world_name},
                        {"site_display_name", settings.site_display_name},
                        {"capacity", settings.capacity}, {"label", static_cast<int>(settings.label)},
                        {"maintenance", settings.maintenance},
                        {"char_creation_restricted", settings.char_creation_restricted}});
    if (!result.at("accepted").get<bool>()) {
      throw std::runtime_error("Login authority refused the realm configuration");
    }
  }

  std::lock_guard lock(realm_mutex_);
  realm_ = settings;
}

bool AuthService::set_account_password(std::int64_t account_id, const std::string &password) {
  if (!is_valid_new_password(password)) {
    throw std::invalid_argument("Password must be 8-256 bytes");
  }
  if (authority_) {
    return authority_account_mutation("set_password", {{"account_id", account_id},
                                                         {"password", password}}, account_id);
  }

  // Derive before touching storage; PBKDF2 is deliberately slow and must not hold a lock.
  const auto salt = random_hex(kPasswordSaltBytes);
  const auto hash = derive_password_hash(password, hex_decode(salt), kDefaultPasswordIterations);

  std::lock_guard lock(mutex_);
  bool updated{};
  try {
    updated = accounts_->set_account_password(account_id, salt, hash, kDefaultPasswordIterations);
  } catch (...) {
    // A lost COMMIT response leaves the durable outcome unknown. Do not keep
    // sessions, tickets or unactivated grants alive under the old password.
    mark_pending_revocation_locked(account_id);
    throw;
  }
  if (!updated) {
    return false;
  }

  // The storage mutation already revoked durable reconnect credentials atomically.
  pending_revocations_.erase(account_id);
  invalidate_account_sessions_locked(account_id);
  return true;
}

bool AuthService::set_account_banned(std::int64_t account_id, bool banned) {
  if (authority_) {
    return authority_account_mutation("set_banned", {{"account_id", account_id},
                                                       {"banned", banned}}, account_id, banned);
  }
  std::lock_guard lock(mutex_);
  bool updated{};
  try {
    updated = accounts_->set_account_banned(account_id, banned);
  } catch (...) {
    // COMMIT may have succeeded despite the error. Fail closed in memory and
    // report the uncertain storage outcome to the operator.
    mark_pending_revocation_locked(account_id);
    throw;
  }
  if (!updated) {
    return false;
  }

  if (banned) {
    // The storage mutation already revoked durable reconnect credentials atomically.
    pending_revocations_.erase(account_id);
    invalidate_account_sessions_locked(account_id);
  }

  return true;
}

void AuthService::revoke_account_sessions(std::int64_t account_id) {
  if (authority_) {
    authority_account_mutation("revoke_account", {{"account_id", account_id}}, account_id);
    return;
  }
  std::lock_guard lock(mutex_);
  revoke_account_sessions_locked(account_id);
}

void AuthService::quarantine_account_sessions(std::int64_t account_id) {
  if (authority_) {
    {
      std::lock_guard lock(mutex_);
      mark_pending_revocation_locked(account_id);
    }
    authority_->call("quarantine_account", {{"account_id", account_id}});
    return;
  }
  std::lock_guard lock(mutex_);
  mark_pending_revocation_locked(account_id);
}

void AuthService::revoke_account_sessions_locked(std::int64_t account_id) {
  try {
    accounts_->revoke_reconnect_credentials(account_id);
  } catch (...) {
    mark_pending_revocation_locked(account_id);
    throw;
  }
  pending_revocations_.erase(account_id);
  invalidate_account_sessions_locked(account_id);
}

void AuthService::mark_pending_revocation_locked(std::int64_t account_id) {
  pending_revocations_.insert(account_id);
  invalidate_account_sessions_locked(account_id);
}

void AuthService::reconcile_pending_revocation_locked(std::int64_t account_id) {
  if (!pending_revocations_.contains(account_id)) {
    return;
  }
  // Keep the deny marker if this operation throws; retry only after an
  // independently authenticated login, never from an old reconnect token.
  accounts_->revoke_reconnect_credentials(account_id);
  pending_revocations_.erase(account_id);
}

void AuthService::invalidate_account_sessions_locked(std::int64_t account_id) {
  std::erase_if(sessions_, [account_id](const auto &entry) {
    return entry.second.account == account_id;
  });
  std::erase_if(tickets_, [account_id](const auto &entry) {
    return entry.second.account_id == account_id;
  });
  std::erase_if(launch_grants_, [account_id](const auto &entry) {
    return entry.second.account == account_id;
  });
  // A world connection may already hold a consumed handoff while it loads the character.
  cancel_world_admission_locked(account_id);
}

void AuthService::replace_world_admission_locked(std::int64_t account_id,
                                                 const Handoff &handoff) {
  std::lock_guard admission_lock(admission_mutex_);
  const auto previous = pending_world_admissions_.find(account_id);
  if (previous != pending_world_admissions_.end()) {
    prepared_world_admissions_.erase({previous->second.authority_id, previous->second.admission_id});
    consumed_authority_handoffs_.erase({previous->second.authority_id, previous->second.admission_id});
  }
  pending_world_admissions_.insert_or_assign(
      account_id, PendingWorldAdmission{handoff.admission_id, handoff.expiry,
                                         handoff.authority_id});
}

void AuthService::cancel_world_admission_locked(std::int64_t account_id) {
  std::lock_guard admission_lock(admission_mutex_);
  const auto pending = pending_world_admissions_.find(account_id);
  if (pending != pending_world_admissions_.end()) {
    prepared_world_admissions_.erase({pending->second.authority_id, pending->second.admission_id});
    consumed_authority_handoffs_.erase({pending->second.authority_id, pending->second.admission_id});
  }
  pending_world_admissions_.erase(account_id);
}

std::map<std::int64_t, unsigned> AuthService::lobby_session_counts() {
  if (authority_) {
    const auto reply = authority_->call("session_counts", nlohmann::json::object());
    std::map<std::int64_t, unsigned> counts;
    for (const auto &entry : reply.at("counts")) {
      counts.emplace(entry.at("account_id").get<std::int64_t>(), entry.at("count").get<unsigned>());
    }
    return counts;
  }
  std::lock_guard lock(mutex_);
  prune_expired();

  std::map<std::int64_t, unsigned> counts;
  for (const auto &[token, session] : sessions_) {
    ++counts[session.account];
  }
  return counts;
}

WorldPopulation::Lease::Lease(std::shared_ptr<std::atomic<unsigned>> counter)
    : counter_(std::move(counter)) {
  if (counter_) {
    counter_->fetch_add(1, std::memory_order_relaxed);
  }
}

WorldPopulation::Lease::Lease(Lease &&other) noexcept
    : counter_(std::exchange(other.counter_, nullptr)) {}

WorldPopulation::Lease &WorldPopulation::Lease::operator=(Lease &&other) noexcept {
  if (this != &other) {
    release();
    counter_ = std::exchange(other.counter_, nullptr);
  }

  return *this;
}

WorldPopulation::Lease::~Lease() {
  release();
}

void WorldPopulation::Lease::release() noexcept {
  if (!counter_) {
    return;
  }

  counter_->fetch_sub(1, std::memory_order_relaxed);
  counter_.reset();
}

WorldPopulation::Lease WorldPopulation::admit() {
  return Lease(counter_);
}

unsigned WorldPopulation::online() const {
  return counter_->load(std::memory_order_relaxed);
}

bool AuthService::create_account(std::string name, const std::string &password) {
  if (authority_) {
    return authority_->call("create_account", {{"name", name}, {"password", password}})
        .at("accepted").get<bool>();
  }
  name = ascii_lower(std::move(name));
  if (!is_valid_account_name(name) || !is_valid_new_password(password)) {
    throw std::invalid_argument(
        "Account must be 3-32 ASCII letters/digits/underscore; password must be 8-256 bytes");
  }
  if (accounts_->account(name)) {
    return false;
  }

  return accounts_->add_account(make_password_account(std::move(name), password));
}

Account AuthService::make_password_account(std::string name, const std::string &password) {
  Account account;
  account.name = name;
  account.salt = random_hex(kPasswordSaltBytes);
  account.hash = derive_password_hash(password, hex_decode(account.salt), account.iterations);
  return account;
}

void AuthService::set_character_social_state(std::shared_ptr<const CharacterSocialState> state) {
  std::lock_guard lock(mutex_);
  social_state_ = std::move(state);
}

bool AuthService::set_account_role(std::string name, AccountRole role) {
  if (authority_) {
    return authority_->call("set_role", {{"name", name}, {"role", static_cast<int>(role)}})
        .at("accepted").get<bool>();
  }
  name = ascii_lower(std::move(name));
  if (!is_valid_account_name(name)) {
    return false;
  }

  return accounts_->set_account_role(name, role);
}

void AuthService::prune_expired() {
  const auto current_time = now_();
  std::erase_if(sessions_, [&](const auto &entry) { return entry.second.expiry <= current_time; });
  std::erase_if(tickets_, [&](const auto &entry) { return entry.second.expiry <= current_time; });
  std::erase_if(launch_grants_,
                [&](const auto &entry) { return entry.second.expiry <= current_time; });

  std::lock_guard admission_lock(admission_mutex_);
  std::erase_if(pending_world_admissions_, [&](const auto &entry) {
    if (entry.second.expiry > current_time) {
      return false;
    }
    prepared_world_admissions_.erase({entry.second.authority_id, entry.second.admission_id});
    consumed_authority_handoffs_.erase({entry.second.authority_id, entry.second.admission_id});
    return true;
  });
}

x5::pa2c::AuthChallengeRes AuthService::login(std::string name, const std::string &password,
                                           bool launcher_session, bool *credential_mismatch) {
  if (authority_) {
    const auto reply = authority_->call("login", {{"name", name}, {"password", password},
                                                   {"launcher", launcher_session}});
    if (credential_mismatch) {
      *credential_mismatch = reply.at("credential_mismatch").get<bool>();
    }
    x5::pa2c::AuthChallengeRes response;
    parse_json(reply.at("response"), response);
    return response;
  }
  if (credential_mismatch) {
    *credential_mismatch = false;
  }
  x5::pa2c::AuthChallengeRes response;
  response.set_ec(x5::ec::LoginDenyAccountMissingOrPasswordNotMatched);
  if (!is_valid_account_name(name) || password.size() > kMaximumPasswordBytes) {
    return response;
  }
  auto account = accounts_->account(ascii_lower(std::move(name)));
  // Derive for nonexistent accounts as well to avoid a cheap account-existence timing oracle.
  auto derived_hash = derive_password_hash(
      password, account ? hex_decode(account->salt) : std::string(kPasswordSaltBytes, '\0'),
      account ? account->iterations : kDefaultPasswordIterations);
  if (!account || account->hash.size() != derived_hash.size() ||
      CRYPTO_memcmp(derived_hash.data(), account->hash.data(), derived_hash.size()) != 0) {
    if (credential_mismatch) {
      *credential_mismatch = true;
    }
    return response;
  }
  return create_login_session(*account, launcher_session);
}

x5::pa2c::AuthChallengeRes AuthService::create_login_session(const Account &account,
                                                            bool launcher_session) {
  x5::pa2c::AuthChallengeRes response;
  response.set_ec(x5::ec::LoginDenyAccountMissingOrPasswordNotMatched);
  if (account.banned) {
    response.set_ec(x5::ec::LoginDenyRestrictedAccount);
    return response;
  }

  std::lock_guard lock(mutex_);
  prune_expired();
  reconcile_pending_revocation_locked(account.id);
  // Password verification and Discord lookup happen before this lock. A reset or ban
  // may have changed the account while they were in progress.
  const auto current_account = accounts_->account_by_id(account.id);
  if (!current_account || current_account->banned || current_account->hash != account.hash ||
      current_account->salt != account.salt || current_account->iterations != account.iterations) {
    return response;
  }

  // A successful new login revokes older sessions and pending WebSocket handoffs.
  std::erase_if(sessions_, [&](const auto &entry) { return entry.second.account == account.id; });
  std::erase_if(tickets_,
                [&](const auto &entry) { return entry.second.account_id == account.id; });
  // A launcher may renew its password session while the game is showing its intro.
  // Keep an already issued, short-lived game grant through that renewal. Explicit
  // sign-out, password reset and account suspension still revoke pending grants.
  if (!launcher_session) {
    accounts_->revoke_reconnect_credentials(account.id);
    std::erase_if(launch_grants_,
                  [&](const auto &entry) { return entry.second.account == account.id; });
  }
  if (sessions_.size() >= kMaximumSessions) {
    response.set_ec(x5::ec::ServerOverload);
    return response;
  }

  auto token = random_hex(kTokenBytes);
  Session session{account.id, 0, now_() + config_.session_ttl, launcher_session};
  session.game_master = current_account->role == AccountRole::game_master;
  sessions_.emplace(token, session);

  response.set_ec(x5::ec::Success);
  response.set_account_id(account.id);
  response.set_access_token(token);
  // AuthChallengeRes.gm: the client stores this flag at login (static research,
  // dumps/ida_research/gm-20261026). The world console is enabled by WorldCharBaseNoti.gm.
  response.set_gm(session.game_master);

  return response;
}

std::optional<std::string> AuthService::issue_launch_grant(const std::string &token) {
  if (authority_) {
    const auto reply = authority_->call("issue_grant", {{"token", token}});
    return reply.at("grant").is_null() ? std::nullopt
                                        : std::optional(reply.at("grant").get<std::string>());
  }
  std::lock_guard lock(mutex_);
  prune_expired();

  const auto current_session = sessions_.find(token);
  if (current_session == sessions_.end()) {
    return std::nullopt;
  }

  const auto account_id = current_session->second.account;
  const auto other_grants = std::count_if(
      launch_grants_.begin(), launch_grants_.end(), [account_id](const auto &entry) {
        return entry.second.account != account_id;
      });
  if (other_grants >= kMaximumLaunchGrants) {
    return std::nullopt;
  }

  // Allocate the replacement before revoking the old credential. An allocation
  // or storage failure must not strand a player with no usable credential.
  const auto expiry = now_() + kLaunchGrantTtl;
  const bool game_master = current_session->second.game_master;
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto grant = random_hex(kTokenBytes);
    const auto [entry, inserted] =
        launch_grants_.emplace(grant, LaunchGrant{account_id, expiry, game_master});
    if (!inserted) {
      continue;
    }

    try {
      accounts_->revoke_reconnect_credentials(account_id);
    } catch (...) {
      launch_grants_.erase(entry);
      throw;
    }
    std::erase_if(launch_grants_, [&](const auto &other) {
      return other.second.account == account_id && other.first != grant;
    });
    return grant;
  }

  throw std::runtime_error("Could not allocate unique launch grant");
}

bool AuthService::valid_launch_grant(std::string_view grant) {
  if (authority_) {
    return authority_->call("valid_grant", {{"grant", grant}}).at("accepted").get<bool>();
  }
  if (grant.size() != kTokenBytes * 2) {
    return false;
  }

  std::lock_guard lock(mutex_);
  prune_expired();

  const auto pending = launch_grants_.find(std::string(grant));
  if (pending != launch_grants_.end()) {
    return !pending_revocations_.contains(pending->second.account);
  }

  const auto account =
      accounts_->reconnect_account(reconnect_credential_digest(grant), unix_time_seconds());
  return account && !pending_revocations_.contains(account->id);
}

std::optional<x5::pa2c::AuthChallengeRes>
AuthService::consume_launch_grant(std::string_view grant) {
  if (authority_) {
    const auto reply = authority_->call("consume_grant", {{"credential", grant}});
    if (reply.is_null()) {
      return std::nullopt;
    }
    x5::pa2c::AuthChallengeRes response;
    parse_json(reply.at("response"), response);
    return response;
  }
  if (grant.size() != kTokenBytes * 2) {
    return std::nullopt;
  }

  std::lock_guard lock(mutex_);
  prune_expired();

  const auto entry = launch_grants_.find(std::string(grant));
  if (entry == launch_grants_.end()) {
    return std::nullopt;
  }

  const auto account_id = entry->second.account;
  if (pending_revocations_.contains(account_id)) {
    return std::nullopt;
  }
  const bool game_master = entry->second.game_master;
  const auto account = accounts_->account_by_id(account_id);
  if (!account || account->banned) {
    launch_grants_.erase(entry);
    return std::nullopt;
  }

  const auto replaced_sessions = std::count_if(
      sessions_.begin(), sessions_.end(), [account_id](const auto &session) {
        return session.second.account == account_id && !session.second.launcher;
      });
  x5::pa2c::AuthChallengeRes response;
  if (sessions_.size() - replaced_sessions >= kMaximumSessions) {
    response.set_ec(x5::ec::ServerOverload);
    return response;
  }

  const auto token = random_hex(kTokenBytes);
  Session session{account_id, 0, now_() + config_.session_ttl};
  session.game_master = game_master;
  response.set_ec(x5::ec::Success);
  response.set_account_id(account_id);
  response.set_access_token(token);
  response.set_gm(game_master);
  const auto [created_session, inserted] = sessions_.emplace(token, session);
  if (!inserted) {
    throw std::runtime_error("Could not allocate unique game session");
  }

  const auto expiration = unix_time_seconds() +
                          std::chrono::duration_cast<std::chrono::seconds>(
                              kReconnectCredentialTtl).count();
  try {
    if (!accounts_->save_reconnect_credential(account_id, reconnect_credential_digest(grant),
                                              expiration)) {
      sessions_.erase(created_session);
      return std::nullopt;
    }
  } catch (...) {
    sessions_.erase(created_session);
    throw;
  }

  // Commit in-memory revocation only after the persistent write succeeded.
  std::erase_if(launch_grants_, [account_id](const auto &other) {
    return other.second.account == account_id;
  });
  std::erase_if(sessions_, [&](const auto &other) {
    return other.first != token && other.second.account == account_id && !other.second.launcher;
  });
  std::erase_if(tickets_, [account_id](const auto &ticket) {
    return ticket.second.account_id == account_id;
  });
  return response;
}

std::optional<x5::pa2c::AuthChallengeRes>
AuthService::reconnect_launch_grant(std::string_view grant) {
  if (authority_) {
    const auto reply = authority_->call("reconnect_grant", {{"credential", grant}});
    if (reply.is_null()) {
      return std::nullopt;
    }
    x5::pa2c::AuthChallengeRes response;
    parse_json(reply.at("response"), response);
    return response;
  }
  if (grant.size() != kTokenBytes * 2) {
    return std::nullopt;
  }

  std::lock_guard lock(mutex_);
  prune_expired();
  const auto account = accounts_->reconnect_account(reconnect_credential_digest(grant),
                                                    unix_time_seconds());
  if (!account || pending_revocations_.contains(account->id)) {
    return std::nullopt;
  }

  std::erase_if(sessions_, [&](const auto &entry) {
    return entry.second.account == account->id && !entry.second.launcher;
  });
  std::erase_if(tickets_, [&](const auto &entry) {
    return entry.second.account_id == account->id;
  });

  x5::pa2c::AuthChallengeRes response;
  if (sessions_.size() >= kMaximumSessions) {
    response.set_ec(x5::ec::ServerOverload);
    return response;
  }

  const auto token = random_hex(kTokenBytes);
  Session session{account->id, 0, now_() + config_.session_ttl};
  session.game_master = account->role == AccountRole::game_master;
  sessions_.emplace(token, session);
  response.set_ec(x5::ec::Success);
  response.set_account_id(account->id);
  response.set_access_token(token);
  response.set_gm(session.game_master);
  return response;
}

std::optional<x5::pa2c::AuthChallengeRes>
AuthService::rotate_session(const std::string &token) {
  if (authority_) {
    const auto reply = authority_->call("rotate_session", {{"credential", token}});
    if (reply.is_null()) {
      return std::nullopt;
    }
    x5::pa2c::AuthChallengeRes response;
    parse_json(reply.at("response"), response);
    return response;
  }
  std::lock_guard lock(mutex_);
  prune_expired();

  const auto current_session = sessions_.find(token);
  if (current_session == sessions_.end()) {
    return std::nullopt;
  }

  const auto account_id = current_session->second.account;
  const bool game_master = current_session->second.game_master;
  std::erase_if(sessions_, [account_id](const auto &entry) {
    return entry.second.account == account_id && !entry.second.launcher;
  });
  std::erase_if(tickets_, [account_id](const auto &entry) {
    return entry.second.account_id == account_id;
  });
  std::erase_if(launch_grants_, [account_id](const auto &entry) {
    return entry.second.account == account_id;
  });

  if (sessions_.size() >= kMaximumSessions) {
    x5::pa2c::AuthChallengeRes response;
    response.set_ec(x5::ec::ServerOverload);
    return response;
  }

  auto replacement_token = random_hex(kTokenBytes);
  Session replacement{account_id, 0, now_() + config_.session_ttl};
  replacement.game_master = game_master;
  sessions_.emplace(replacement_token, replacement);

  x5::pa2c::AuthChallengeRes response;
  response.set_ec(x5::ec::Success);
  response.set_account_id(account_id);
  response.set_access_token(replacement_token);
  response.set_gm(game_master);
  return response;
}

AuthService::Session AuthService::require_session(const std::string &token, bool in_world) {
  prune_expired();
  if (authority_) {
    return authority_session(token, in_world);
  }
  auto session_iterator = sessions_.find(token);
  if (session_iterator == sessions_.end() ||
      (in_world && session_iterator->second.world != config_.world_id)) {
    throw Unauthorized();
  }
  return session_iterator->second;
}

bool AuthService::authenticated(const std::string &token) {
  std::lock_guard lock(mutex_);
  try {
    require_session(token);
    return true;
  } catch (const Unauthorized &) {
    return false;
  }
}

std::optional<ChatIdentity> AuthService::chat_identity(const std::string &token) {
  std::lock_guard lock(mutex_);
  return require_session(token, true).selected_character;
}

std::optional<Character> AuthService::selected_character(const std::string &token) {
  std::lock_guard lock(mutex_);
  const auto session = require_session(token, true);
  if (!session.selected_character) {
    return std::nullopt;
  }

  const auto characters = characters_->characters(session.account, session.world);
  const auto *character =
      find_owned_character(characters, session.selected_character->character_id);
  if (!character) {
    return std::nullopt;
  }
  return *character;
}

std::optional<AuthService::CharacterOwner>
AuthService::character_owner(const std::string &token, std::int64_t character_id,
                             bool selected_only) {
  std::lock_guard lock(mutex_);
  const auto session = require_session(token, true);
  const bool selected =
      session.selected_character && session.selected_character->character_id == character_id;
  // The client can request the hot-key load from the lobby, before CharSelectReq (observed on
  // the hosted server), and does not repeat it after selection. Callers that read are
  // therefore accepted for any character of the session's account and realm; callers that
  // write (and exploration) still require the selection.
  if (!selected) {
    if (selected_only) {
      return std::nullopt;
    }
    const auto owned = characters_->characters(session.account, session.world);
    const bool owns_character = std::ranges::any_of(
        owned, [&](const Character &character) { return character.id == character_id; });
    if (!owns_character) {
      return std::nullopt;
    }
  }
  return CharacterOwner{session.account, session.world};
}

void AuthService::logout(const std::string &token) {
  if (authority_) {
    authority_->call("logout", {{"token", token}});
    std::lock_guard lock(mutex_);
    sessions_.erase(token);
    return;
  }
  std::lock_guard lock(mutex_);
  prune_expired();
  const auto session = sessions_.find(token);
  if (session == sessions_.end()) {
    return;
  }

  const auto account_id = session->second.account;
  if (session->second.launcher) {
    // Signing out of the desktop must not revoke a game that already consumed its own grant.
    sessions_.erase(session);
    std::erase_if(launch_grants_, [account_id](const auto &entry) {
      return entry.second.account == account_id;
    });
    return;
  }
  std::erase_if(sessions_, [account_id](const auto &entry) {
    return entry.second.account == account_id && !entry.second.launcher;
  });
  std::erase_if(tickets_, [&](const auto &entry) { return entry.second.account_id == account_id; });
  std::erase_if(launch_grants_, [account_id](const auto &entry) {
    return entry.second.account == account_id;
  });
}

bool AuthService::bind_local_session(const std::string &valid_local_token,
                                     const std::string &presented_token) {
  if (authority_) {
    return authority_->call("bind_session", {{"valid_token", valid_local_token},
                                               {"presented_token", presented_token}})
        .at("accepted").get<bool>();
  }
  if (presented_token.empty() || presented_token.size() > kMaximumSessionTokenBytes) {
    return false;
  }

  std::lock_guard lock(mutex_);
  prune_expired();
  const auto local_session = sessions_.find(valid_local_token);
  if (local_session == sessions_.end()) {
    return false;
  }

  const auto existing_alias = sessions_.find(presented_token);
  if (existing_alias != sessions_.end()) {
    return existing_alias->second.account == local_session->second.account;
  }
  if (sessions_.size() >= kMaximumSessions) {
    return false;
  }

  // The opt-in local adapter has already authenticated emulator credentials. The presented
  // opaque value is only a process-local alias; it is never validated as a platform credential.
  const auto session_copy = local_session->second;
  sessions_.emplace(presented_token, session_copy);
  return true;
}

x5::pb::ServerState AuthService::current_world_state(unsigned online,
                                                     const RealmSettings &realm) const {
  if (realm.maintenance) {
    return x5::pb::ServerState_GmOnly;
  }
  if (online >= realm.capacity) {
    return x5::pb::ServerState_Overloaded;
  }

  // Widen before multiplying so a large capacity cannot overflow the comparison.
  const auto scaled_online = static_cast<std::uint64_t>(online) * 100;
  const auto congested_threshold =
      static_cast<std::uint64_t>(realm.capacity) * kCongestedPopulationPercent;
  if (scaled_online >= congested_threshold) {
    return x5::pb::ServerState_Congested;
  }

  return x5::pb::ServerState_Normal;
}

WorldStatus AuthService::world_status() const {
  const auto realm = realm_settings();

  WorldStatus status;
  status.world_id = config_.world_id;
  status.name = realm.world_name;
  status.online = population_.online();
  status.capacity = realm.capacity;
  status.state = current_world_state(status.online, realm);
  status.char_creation_restricted = realm.char_creation_restricted;

  if (world_heartbeat_required_) {
    constexpr auto kMaximumHeartbeatAge = std::chrono::seconds(5);
    std::lock_guard lock(world_heartbeat_mutex_);
    if (!world_heartbeat_ || now_() - world_heartbeat_->second > kMaximumHeartbeatAge) {
      status.online = 0;
      status.state = x5::pb::ServerState_OffLine;
    } else {
      status.online = world_heartbeat_->first;
      status.state = current_world_state(status.online, realm);
    }
  }

  return status;
}

WorldPopulation::Lease AuthService::admit_world_character() {
  return population_.admit();
}

x5::pa2c::WorldListRes AuthService::public_worlds() const {
  const auto status = world_status();
  const auto realm = realm_settings();

  x5::pa2c::WorldListRes response;
  response.set_error_code(x5::ec::Success);
  response.set_region("local");
  auto *site = response.add_sites();
  site->set_name(config_.site_name);
  site->set_display_name(realm.site_display_name);
  auto *world = response.add_worlds();
  world->set_world_id(status.world_id);
  world->set_site_name(config_.site_name);
  world->set_world_name(status.name);
  world->set_version("development");
  world->set_state(status.state);
  world->set_label(realm.label);
  world->set_char_creation_restricted(status.char_creation_restricted);

  return response;
}

x5::pa2c::WorldListRes AuthService::worlds(const std::string &token) {
  std::lock_guard lock(mutex_);
  const auto session = require_session(token);
  auto response = public_worlds();
  const auto character_count =
      static_cast<int>(characters_->characters(session.account, config_.world_id).size());
  (*response.mutable_world_character_counts())[config_.world_id] = character_count;
  // The emulator has one realm and keeps no entry history, so an account that owns characters
  // there has necessarily last played on it.
  if (character_count > 0) {
    response.set_last_world_id(config_.world_id);
  }

  return response;
}

x5::pa2c::WorldEnterRes AuthService::enter(const std::string &token,
                                           const x5::pb::authwebsocket::WorldEnterReq &request) {
  if (authority_) {
    const auto reply = authority_->call("enter", {{"token", token},
                                                   {"request", message_json(request)}});
    x5::pa2c::WorldEnterRes response;
    parse_json(reply.at("response"), response);
    return response;
  }
  std::lock_guard lock(mutex_);
  const auto session = require_session(token);
  const auto realm = realm_settings();
  x5::pa2c::WorldEnterRes response;
  response.set_world_id(request.world_id());
  if (request.world_id() != config_.world_id) {
    response.set_error_code(x5::ec::InvalidWorld);
    return response;
  }
  const auto status = world_status();
  if (status.state == x5::pb::ServerState_OffLine) {
    response.set_error_code(x5::ec::WorldIsUnderMaintenance);
    return response;
  }
  // Maintenance lists the realm as ServerState_GmOnly, so game masters may still enter.
  // Letting them past the capacity limit is emulator policy, not a recovered official rule.
  if ((realm.maintenance || status.state == x5::pb::ServerState_GmOnly) && !session.game_master) {
    response.set_error_code(x5::ec::WorldIsUnderMaintenance);
    return response;
  }
  // Admission is checked at realm entry. Characters already admitted to the lobby may still
  // connect, so the world can briefly exceed capacity by the players selecting a character.
  if (status.online >= status.capacity && !session.game_master) {
    response.set_error_code(x5::ec::ServerOverload);
    return response;
  }

  sessions_.at(token).world = request.world_id();
  sessions_.at(token).selected_character.reset();
  response.set_error_code(x5::ec::Success);
  auto *granted = response.mutable_granted();
  granted->set_front_rest_address(config_.lobby_address);
  granted->set_allow_char_create(!realm.char_creation_restricted);

  return response;
}

x5::pb::frontwebsocket::CharListRes AuthService::list(const std::string &token) {
  std::lock_guard lock(mutex_);
  auto session = require_session(token, true);
  x5::pb::frontwebsocket::CharListRes response;

  response.set_ec(x5::ec::Success);
  response.set_site_name(config_.site_name);
  for (const auto &character : characters_->characters(session.account, session.world)) {
    auto *lobby_character = response.add_char_list();
    *lobby_character = make_lobby_character(character);
    if (social_state_) {
      social_state_->decorate_lobby(character.id, *lobby_character);
    }
  }

  return response;
}

x5::pb::frontwebsocket::FriendListRes
AuthService::friends(const std::string &token, const x5::pb::ws::front::FriendListReq &request) {
  std::lock_guard lock(mutex_);
  require_session(token, true);

  // Friend relationships are not stored yet. Preserve the requested list context
  // even when its optional flag is explicitly false and the returned list is empty.
  x5::pb::frontwebsocket::FriendListRes response;
  if (request.has_recommend()) {
    response.set_recommend(request.recommend());
  }

  return response;
}

x5::pb::frontwebsocket::CharCreateRes
AuthService::create(const std::string &token, const x5::pb::ws::front::CharCreateReq &request) {
  std::lock_guard lock(mutex_);
  auto session = require_session(token, true);
  x5::pb::frontwebsocket::CharCreateRes response;
  if (realm_settings().char_creation_restricted) {
    response.set_ec(x5::ec::CharCreationRestricted);
    return response;
  }
  if (!request.has_slot() || request.slot() >= kCharacterSlotCount) {
    response.set_ec(x5::ec::CharCreateMaxLimit);
    return response;
  }
  if (!request.has_char_name() || request.char_name().size() < kMinimumCharacterNameLength ||
      request.char_name().size() > kMaximumCharacterNameLength) {
    response.set_ec(x5::ec::CharCreateNameLengthInvalid);
    return response;
  }
  if (!std::all_of(request.char_name().begin(), request.char_name().end(),
                   [](char character) { return is_ascii_alphanumeric(character); })) {
    response.set_ec(x5::ec::CharCreateNameInvalid);
    return response;
  }

  auto class_id = static_cast<int>(request.char_class());
  if (starter_gear_for_class(class_id).empty()) {
    // Only classes with a catalog-backed starter set may be created. Any other
    // class would spawn with no weapon, which the client renders as a default
    // item.
    response.set_ec(x5::ec::BadRequest);
    return response;
  }

  Character character;
  character.account_id = session.account;
  character.world_id = session.world;
  character.slot = request.slot();
  character.name = request.char_name();
  character.class_id = class_id;
  character.customization_hex = hex_encode(request.char_customization().SerializeAsString());
  try {
    character = characters_->create(std::move(character));
    response.set_ec(x5::ec::Success);
    *response.mutable_char_() = make_lobby_character(character);
  } catch (const CreateError &error) {
    response.set_ec(error.reason == CreateFailure::name_used ? x5::ec::CharCreateNameAlreadyUsed
                                                             : x5::ec::CharCreateMaxLimit);
  }

  return response;
}

x5::pb::frontwebsocket::CharSelectRes
AuthService::select(const std::string &token, const x5::pb::ws::front::CharSelectReq &request) {
  std::lock_guard lock(mutex_);
  auto session = require_session(token, true);
  x5::pb::frontwebsocket::CharSelectRes response;
  auto characters = characters_->characters(session.account, session.world);
  auto selected_character =
      std::find_if(characters.begin(), characters.end(),
                   [&](const auto &character) { return character.id == request.char_id(); });
  if (selected_character == characters.end()) {
    response.set_ec(x5::ec::InvalidChar);
    return response;
  }
  std::erase_if(tickets_,
                [&](const auto &entry) { return entry.second.account_id == session.account; });

  auto handoff_token = random_hex(kTokenBytes);
  auto packet_key = hex_decode(random_hex(kPacketKeyBytes));
  Handoff handoff{session.account, selected_character->id, session.world, packet_key,
                  now_() + config_.ticket_ttl};
  handoff.game_master = session.game_master;
  if (authority_) {
    handoff = register_authority_handoff(token, selected_character->id, handoff_token, packet_key);
    if (handoff.account_id != session.account) {
      throw Unauthorized();
    }
    replace_world_admission_locked(session.account, handoff);
  } else {
    handoff.admission_id = ++last_world_admission_id_;
    // Selection supersedes older pending or consumed handoffs of this account.
    replace_world_admission_locked(session.account, handoff);
    tickets_.emplace(handoff_token, std::move(handoff));
  }
  sessions_.at(token).selected_character =
      ChatIdentity{session.world, selected_character->id, selected_character->name};

  response.set_ec(x5::ec::Success);
  response.set_char_token(handoff_token);
  // Synthetic handoff key: never claim compatibility with the original client's key exchange.
  if (!config_.game_address.empty()) {
    response.set_game_address(config_.game_address);
    response.set_packet_key(packet_key);
  }

  auto *character = response.mutable_character();
  character->set_char_id(selected_character->id);
  character->set_name(selected_character->name);
  character->set_char_class(static_cast<x5::cms::EX5CharacterClass>(selected_character->class_id));
  character->set_level(selected_character->level);
  if (social_state_) {
    social_state_->decorate_world(selected_character->id, *character);
  }

  return response;
}

const Character *AuthService::find_owned_character(
    const std::vector<Character> &characters, std::int64_t character_id) {
  const auto owned = std::find_if(characters.begin(), characters.end(), [&](const auto &character) {
    return character.id == character_id;
  });
  return owned == characters.end() ? nullptr : &*owned;
}

x5::pb::frontwebsocket::CharDeletePreCheckRes AuthService::delete_precheck(
    const std::string &token, const x5::pb::ws::front::CharDeletePreCheckReq &request) {
  std::lock_guard lock(mutex_);
  auto session = require_session(token, true);

  x5::pb::frontwebsocket::CharDeletePreCheckRes response;
  response.set_char_id(request.char_id());

  const auto characters = characters_->characters(session.account, session.world);
  const auto *character = find_owned_character(characters, request.char_id());
  if (character == nullptr) {
    response.set_ec(x5::ec::InvalidChar);
    return response;
  }

  // Guild and housing blockers come from their services; market blockers are not modeled.
  // delete_character itself still refuses until pending deletion is modeled.
  response.set_ec(x5::ec::Success);
  response.set_level(static_cast<std::uint32_t>(character->level));
  response.set_name(character->name);
  if (social_state_) {
    const auto check = social_state_->deletion_check(character->id);
    for (const auto reason : check.block_reasons) {
      response.add_block_reasons(reason);
    }
    // Candidate: a plain guild member has no CharDeleteBlockReason value, so the precheck
    // reports CharDeleteGuildMember as its error code to warn before the name is typed.
    if (check.block_reasons.empty() && check.block != x5::ec::Success) {
      response.set_ec(check.block);
    }
  }
  return response;
}

x5::pb::frontwebsocket::CharDeleteRes
AuthService::delete_character(const std::string &token,
                              const x5::pb::ws::front::CharDeleteReq &request) {
  std::lock_guard lock(mutex_);
  auto session = require_session(token, true);

  x5::pb::frontwebsocket::CharDeleteRes response;
  response.set_char_id(request.char_id());

  const auto characters = characters_->characters(session.account, session.world);
  const auto *character = find_owned_character(characters, request.char_id());
  if (character == nullptr) {
    response.set_ec(x5::ec::InvalidChar);
    return response;
  }
  if (character->name != request.char_name()) {
    response.set_ec(x5::ec::CharDeleteNameMismatch);
    return response;
  }
  if (social_state_) {
    const auto check = social_state_->deletion_check(character->id);
    if (check.block != x5::ec::Success) {
      response.set_ec(check.block);
      return response;
    }
  }

  // Fail closed: the official flow schedules a pending deletion (blockers,
  // grace window, delete_scheduled_epochtime, CharDeleteAlreadyPending) that
  // cannot be modeled without persisted pending state and blocker data.
  // Refuse rather than remove character rows with unverified policy.
  response.set_ec(x5::ec::CharDeleteInvalid);
  return response;
}

x5::pb::frontwebsocket::CharDeleteCancelRes AuthService::delete_cancel(
    const std::string &token, const x5::pb::ws::front::CharDeleteCancelReq &request) {
  std::lock_guard lock(mutex_);
  auto session = require_session(token, true);

  x5::pb::frontwebsocket::CharDeleteCancelRes response;
  response.set_char_id(request.char_id());

  const auto characters = characters_->characters(session.account, session.world);
  if (find_owned_character(characters, request.char_id()) == nullptr) {
    response.set_ec(x5::ec::InvalidChar);
    return response;
  }

  // No character can be pending while deletion itself is refused.
  response.set_ec(x5::ec::CharDeleteNotPending);
  return response;
}

x5::pb::frontwebsocket::CharDeleteImmediateRes AuthService::delete_immediate(
    const std::string &token, const x5::pb::ws::front::CharDeleteImmediateReq &request) {
  std::lock_guard lock(mutex_);
  auto session = require_session(token, true);

  x5::pb::frontwebsocket::CharDeleteImmediateRes response;
  response.set_char_id(request.char_id());

  const auto characters = characters_->characters(session.account, session.world);
  if (find_owned_character(characters, request.char_id()) == nullptr) {
    response.set_ec(x5::ec::InvalidChar);
    return response;
  }

  // Without a pending-delete record there is nothing to accelerate; keep data.
  response.set_ec(x5::ec::CharDeleteNotPending);
  return response;
}

x5::pb::frontwebsocket::LookupCharIdByNameRes AuthService::lookup_char_id(
    const std::string &token, const x5::pb::ws::front::LookupCharIdByNameReq &request) {
  std::lock_guard lock(mutex_);
  auto session = require_session(token, true);

  x5::pb::frontwebsocket::LookupCharIdByNameRes response;
  response.set_char_name(request.char_name());

  if (request.char_name().size() < kMinimumCharacterNameLength ||
      request.char_name().size() > kMaximumCharacterNameLength) {
    response.set_ec(x5::ec::BadRequest);
    return response;
  }

  // WSC2F_LookupCharIdByNameReq carries only a name, so the lookup is not scoped to the
  // caller's account. It is limited to the realm the session entered (exact, case-sensitive
  // match) and returns only the character ID.
  const auto character = characters_->character_summary_by_name(session.world, request.char_name());
  if (!character.has_value()) {
    response.set_ec(x5::ec::InvalidChar);
    return response;
  }

  response.set_ec(x5::ec::Success);
  response.set_char_id(character->id);
  return response;
}


std::optional<Handoff> AuthService::consume_ticket(const std::string &token) {
  std::lock_guard lock(mutex_);
  if (authority_) {
    return consume_authority_handoff("token", token, {}, nullptr);
  }
  prune_expired();
  auto node = tickets_.extract(token);
  if (node.empty()) {
    return {};
  }
  return std::move(node.mapped());
}

std::optional<Handoff> AuthService::consume_packet_key(
    std::string_view packet_key, const std::function<bool(const Handoff &)> &admit,
    bool *admission_busy) {
  if (admission_busy) {
    *admission_busy = false;
  }
  if (packet_key.size() != kPacketKeyBytes) {
    return {};
  }

  std::lock_guard lock(mutex_);
  if (authority_) {
    return consume_authority_handoff("packet_key", packet_key, admit, admission_busy);
  }
  prune_expired();
  const auto ticket = std::find_if(tickets_.begin(), tickets_.end(), [&](const auto &entry) {
    const auto &expected_key = entry.second.packet_key;
    return expected_key.size() == packet_key.size() &&
           CRYPTO_memcmp(expected_key.data(), packet_key.data(), packet_key.size()) == 0;
  });
  if (ticket == tickets_.end()) {
    return {};
  }

  if (admit && !admit(ticket->second)) {
    if (admission_busy) {
      *admission_busy = true;
    }
    return {};
  }

  // Both native key presentation and development token presentation consume the same handoff.
  auto handoff = std::move(ticket->second);
  tickets_.erase(ticket);
  return handoff;
}

std::optional<Handoff> AuthService::consume_world_token(
    std::string_view char_token, const std::function<bool(const Handoff &)> &admit,
    bool *admission_busy) {
  if (admission_busy) {
    *admission_busy = false;
  }
  if (char_token.size() != kTokenBytes * 2 ||
      !std::all_of(char_token.begin(), char_token.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
      })) {
    return {};
  }

  std::lock_guard lock(mutex_);
  if (authority_) {
    return consume_authority_handoff("token", char_token, admit, admission_busy);
  }
  prune_expired();

  const auto ticket = std::find_if(tickets_.begin(), tickets_.end(), [&](const auto &entry) {
    return entry.first.size() == char_token.size() &&
           CRYPTO_memcmp(entry.first.data(), char_token.data(), char_token.size()) == 0;
  });
  if (ticket == tickets_.end()) {
    return {};
  }

  if (admit && !admit(ticket->second)) {
    if (admission_busy) {
      *admission_busy = true;
    }
    return {};
  }

  auto handoff = std::move(ticket->second);
  tickets_.erase(ticket);
  return handoff;
}

bool AuthService::claim_world_admission(const Handoff &handoff) {
  // The ticket lifetime also bounds how long a consumed handoff may wait for admission.
  if (handoff.expiry <= now_()) {
    return false;
  }

  // This is the admission linearization point. Selection and revocation replace or erase the
  // entry under the same mutex, so whichever happens first decides the outcome.
  std::lock_guard admission_lock(admission_mutex_);
  const auto pending = pending_world_admissions_.find(handoff.account_id);
  if (pending == pending_world_admissions_.end() ||
      pending->second.admission_id != handoff.admission_id ||
      pending->second.authority_id != handoff.authority_id) {
    return false;
  }

  if (authority_ &&
      prepared_world_admissions_.erase({handoff.authority_id, handoff.admission_id}) != 1) {
    return false;
  }
  consumed_authority_handoffs_.erase({handoff.authority_id, handoff.admission_id});

  pending_world_admissions_.erase(pending);
  return true;
}

std::optional<Character> AuthService::world_character(const Handoff &handoff) {
  const auto characters = characters_->characters(handoff.account_id, handoff.world_id);
  const auto selected = std::find_if(characters.begin(), characters.end(),
                                     [&](const Character &character) {
                                       return character.id == handoff.character_id;
                                     });
  if (selected == characters.end()) {
    return std::nullopt;
  }

  return *selected;
}

bool AuthService::update_world_items(const Handoff &handoff, std::int64_t expected_revision,
                                     const std::vector<CharacterItem> &items) {
  return character_state_->update_items(handoff.account_id, handoff.character_id,
                                        handoff.world_id, expected_revision, items);
}

bool AuthService::commit_world_death(
    const Handoff &handoff, std::int64_t expected_revision,
    const std::vector<EquippedDurabilityChange> &durability_changes) {
  return character_state_->commit_world_death(handoff.account_id, handoff.character_id,
                                              handoff.world_id, expected_revision,
                                              durability_changes);
}

bool AuthService::commit_world_revival(const Handoff &handoff, std::int64_t expected_revision,
                                       const WorldPosition &position) {
  return character_state_->commit_world_revival(handoff.account_id, handoff.character_id,
                                                handoff.world_id, expected_revision, position);
}

bool AuthService::commit_world_shrine_revival(const Handoff &handoff,
                                             const WorldProgress &before,
                                             const WorldProgress &after,
                                             const WorldPosition &position) {
  return character_state_->commit_world_shrine_revival(handoff.account_id, handoff.character_id,
                                                       handoff.world_id, before, after, position);
}

bool AuthService::discard_world_items(const Handoff &handoff, std::int64_t expected_revision,
                                      const std::vector<std::int64_t> &item_ids,
                                      const std::map<int, int> &stackable_counts) {
  return character_state_->discard_items(handoff.account_id, handoff.character_id,
                                         handoff.world_id, expected_revision, item_ids,
                                         stackable_counts);
}

bool AuthService::apply_world_item_polish(const Handoff &handoff, std::int64_t expected_revision,
                                         const ItemPolishChange &change) {
  return character_state_->apply_item_polish(handoff.account_id, handoff.character_id,
                                             handoff.world_id, expected_revision, change);
}

bool AuthService::save_world_position(const Handoff &handoff,
                                      const WorldPosition &position) {
  return characters_->save_world_position(handoff.account_id, handoff.character_id,
                                          handoff.world_id, position);
}

WorldProgress AuthService::world_progress(const Handoff &handoff) {
  return character_state_->load_world_progress(handoff.account_id, handoff.character_id,
                                               handoff.world_id);
}

bool AuthService::save_world_progress(const Handoff &handoff, const WorldProgress &before,
                                      const WorldProgress &after) {
  return character_state_->save_world_progress(handoff.account_id, handoff.character_id,
                                               handoff.world_id, before, after);
}

std::vector<std::int64_t> AuthService::offline_admin_quarantines() {
  return character_state_->offline_admin_quarantines();
}

bool AuthService::fence_world_entry(const Handoff &handoff) {
  return character_state_->fence_world_entry(handoff.account_id, handoff.character_id,
                                             handoff.world_id);
}

std::optional<DeathReconciliationSnapshot> AuthService::fence_world_death_reconciliation(
    const Handoff &handoff) {
  return character_state_->fence_world_death_reconciliation(
      handoff.account_id, handoff.character_id, handoff.world_id);
}

bool AuthService::commit_world_progress_and_position(const Handoff &handoff,
                                                      const WorldProgress &before,
                                                      const WorldProgress &after,
                                                      const WorldPosition &position) {
  return character_state_->commit_world_progress_and_position(
      handoff.account_id, handoff.character_id, handoff.world_id, before, after, position);
}

std::optional<std::vector<CharacterItem>>
AuthService::commit_world_item_trade(const Handoff &handoff, const WorldProgress &before,
                                     const WorldProgress &after,
                                     const ItemTrade &trade) {
  return character_state_->commit_item_trade(handoff.account_id, handoff.character_id,
                                             handoff.world_id, before, after, trade);
}

std::optional<QuestServiceMembership> AuthService::read_quest_service_membership(
    const Handoff &handoff) {
  return character_state_->read_quest_service_membership(handoff.account_id, handoff.character_id,
                                                         handoff.world_id);
}

std::optional<std::vector<CharacterItem>> AuthService::commit_unaffiliated_activity(
    const Handoff &handoff, const QuestServiceMembership &membership,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade) {
  return character_state_->commit_unaffiliated_activity(
      handoff.account_id, handoff.character_id, handoff.world_id, membership, before, after, trade);
}

QuestMutationResult AuthService::commit_quest_mutation(
    const Handoff &handoff, const std::string &effect_id, const WorldProgress &before,
    const std::optional<WorldProgress> &after, const ItemTrade &trade,
    const QuestServiceEffects &effects) {
  return character_state_->commit_quest_mutation(handoff.account_id, handoff.character_id,
                                                handoff.world_id, effect_id, before, after,
                                                trade, effects);
}

QuestMutationResult AuthService::commit_combat_quest_event(
    const Handoff &handoff, const std::string &operation_id, const std::string &source_fingerprint,
    const WorldProgress &before, const std::optional<WorldProgress> &after, const ItemTrade &trade,
    const QuestServiceEffects &effects) {
  return character_state_->commit_combat_quest_event(
      handoff.account_id, handoff.character_id, handoff.world_id, operation_id, source_fingerprint,
      before, after, trade, effects);
}

MonsterRewardCommitResult AuthService::commit_monster_reward_with_effects(
    const Handoff &handoff, const std::string &receipt_id, const WorldProgress &before,
    const std::optional<WorldProgress> &after, const ItemTrade &trade,
    const QuestServiceEffects &effects) {
  return character_state_->commit_monster_reward_with_effects(
      handoff.account_id, handoff.character_id, handoff.world_id, receipt_id, before, after,
      trade, effects);
}

LootClaimResult AuthService::lookup_world_loot_claim(const Handoff &handoff,
                                                       const std::string &receipt_id,
                                                       const std::string &corpse_fingerprint) {
  return character_state_->lookup_loot_claim(handoff.account_id, handoff.character_id,
                                             handoff.world_id, receipt_id, corpse_fingerprint);
}

LootClaimResult AuthService::commit_world_loot_claim(
    const Handoff &handoff, const std::string &receipt_id, const std::string &corpse_fingerprint,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade) {
  return character_state_->commit_loot_claim(handoff.account_id, handoff.character_id,
                                             handoff.world_id, receipt_id, corpse_fingerprint,
                                             before, after, trade);
}

LootClaimResult AuthService::commit_gathering_quest_claim(
    const Handoff &handoff, const std::string &receipt_id,
    const std::string &source_fingerprint, const WorldProgress &before,
    const WorldProgress &after, const ItemTrade &trade, const QuestServiceEffects &effects,
    const std::optional<SharedGatheringSourceTransition> &source) {
  return character_state_->commit_gathering_quest_claim(
      handoff.account_id, handoff.character_id, handoff.world_id, receipt_id,
      source_fingerprint, before, after, trade, effects, source);
}

GatheringFinalizeResult AuthService::finalize_gathering_quest_claim(
    const Handoff &handoff, const std::string &receipt_id,
    const std::string &source_fingerprint, const WorldProgress &before,
    const WorldProgress &after, const ItemTrade &trade, const QuestServiceEffects &effects,
    const std::optional<SharedGatheringSourceTransition> &source,
    std::int64_t confirmed_epoch_ticks) {
  return character_state_->finalize_gathering_quest_claim(
      handoff.account_id, handoff.character_id, handoff.world_id, receipt_id,
      source_fingerprint, before, after, trade, effects, source, confirmed_epoch_ticks);
}

std::vector<SharedGatheringSourceRecord> AuthService::shared_gathering_sources(
    int world_id, const std::optional<SharedGatheringSourceKey> &after, std::size_t limit) {
  return character_state_->shared_gathering_sources(world_id, after, limit);
}

SharedGatheringSourceUpdateResult AuthService::fence_shared_gathering_source(
    int world_id, const SharedGatheringSourceTransition &initial) {
  return character_state_->fence_shared_gathering_source(world_id, initial);
}

SharedGatheringSourceUpdateResult AuthService::update_shared_gathering_source(
    int world_id, const SharedGatheringSourceTransition &transition,
    std::int64_t epoch_ticks, bool require_due) {
  return character_state_->update_shared_gathering_source(world_id, transition, epoch_ticks,
                                                           require_due);
}

bool AuthService::enqueue_monster_rewards(const std::vector<MonsterRewardReceipt> &receipts) {
  return character_state_->enqueue_monster_rewards(receipts);
}

bool AuthService::enqueue_monster_rewards_with_script(
    const std::vector<MonsterRewardReceipt> &receipts,
    const std::optional<BossScriptIntent> &script) {
  if (!script) {
    return character_state_->enqueue_monster_rewards(receipts);
  }
  return character_state_->enqueue_monster_rewards_with_script(receipts, script);
}

std::vector<BossScriptIntent> AuthService::pending_boss_scripts(
    int world_id, const std::optional<std::string> &after, std::size_t limit) {
  return character_state_->pending_boss_scripts(world_id, after, limit);
}

BossScriptSettlement AuthService::settle_shared_boss_script(const BossScriptIntent &intent) {
  return character_state_->settle_shared_boss_script(intent);
}

bool AuthService::mark_boss_script_published(const BossScriptIntent &intent) {
  return character_state_->mark_boss_script_published(intent);
}

bool AuthService::mark_private_boss_script_applied(const BossScriptIntent &intent) {
  return character_state_->mark_private_boss_script_applied(intent);
}

bool AuthService::retire_private_boss_script(const BossScriptIntent &intent) {
  return character_state_->retire_private_boss_script(intent);
}

std::int64_t AuthService::acquire_monster_reward_owner_epoch() {
  return character_state_->acquire_monster_reward_owner_epoch();
}

void AuthService::adopt_monster_reward_owner_epoch(std::int64_t epoch) {
  character_state_->adopt_monster_reward_owner_epoch(epoch);
}

bool AuthService::mark_monster_reward_ready(const std::string &receipt_id) {
  return character_state_->mark_monster_reward_ready(receipt_id);
}

std::size_t AuthService::recover_staged_monster_rewards(std::size_t limit) {
  return character_state_->recover_staged_monster_rewards(limit);
}

bool AuthService::has_staged_monster_rewards() {
  return character_state_->has_staged_monster_rewards();
}

std::vector<MonsterRewardReceipt> AuthService::pending_monster_rewards(const Handoff &handoff) {
  return character_state_->pending_monster_rewards(handoff.account_id, handoff.character_id,
                                                   handoff.world_id);
}

MonsterRewardCommitResult AuthService::commit_monster_reward(
    const Handoff &handoff, const std::string &receipt_id, const WorldProgress &before,
    const std::optional<WorldProgress> &after, const ItemTrade &trade) {
  return character_state_->commit_monster_reward(handoff.account_id, handoff.character_id,
                                                handoff.world_id, receipt_id, before, after, trade);
}

WorldCredentialDiagnostic AuthService::diagnose_world_credential(std::string_view received) {
  std::lock_guard lock(mutex_);
  prune_expired();

  WorldCredentialDiagnostic diagnostic;
  for (const auto &[token, handoff] : tickets_) {
    const auto key_match = longest_credential_prefix_match(received, handoff.packet_key);
    if (key_match.length > diagnostic.packet_key.length) {
      diagnostic.packet_key = key_match;
    }

    const auto token_match = longest_credential_prefix_match(received, token);
    if (token_match.length > diagnostic.char_token.length) {
      diagnostic.char_token = token_match;
    }
  }

  return diagnostic;
}
} // namespace chronicles
