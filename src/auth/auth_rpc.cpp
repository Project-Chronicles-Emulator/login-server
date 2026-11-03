#include "auth/auth.hpp"
#include "auth/auth_authority.hpp"

#include <algorithm>
#include <limits>
#include <openssl/crypto.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kWorldTokenBytes = 64;
constexpr std::size_t kPacketKeyBytes = 32;

Json encode_handoff(const Handoff &handoff, Clock::time_point now) {
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(handoff.expiry - now).count();
  return {{"account_id", handoff.account_id},
          {"character_id", handoff.character_id},
          {"world_id", handoff.world_id},
          {"packet_key_hex", hex_encode(handoff.packet_key)},
          {"remaining_ms", std::max<std::int64_t>(0, remaining)},
          {"game_master", handoff.game_master},
          {"admission_id", handoff.admission_id},
          {"authority_id", handoff.authority_id}};
}

Handoff decode_handoff(const Json &value, Clock::time_point request_started,
                       std::chrono::seconds maximum_ttl) {
  Handoff result;
  result.account_id = value.at("account_id").get<std::int64_t>();
  result.character_id = value.at("character_id").get<std::int64_t>();
  result.world_id = value.at("world_id").get<int>();
  result.packet_key = hex_decode(value.at("packet_key_hex").get<std::string>());
  result.game_master = value.at("game_master").get<bool>();
  result.admission_id = value.at("admission_id").get<std::uint64_t>();
  result.authority_id = value.at("authority_id").get<std::string>();
  const auto remaining = value.at("remaining_ms").get<std::int64_t>();
  const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(maximum_ttl).count();
  if (result.account_id <= 0 || result.character_id <= 0 || result.world_id <= 0 ||
      result.packet_key.size() != kPacketKeyBytes || result.admission_id == 0 || remaining <= 0 ||
      remaining > maximum || result.authority_id.size() != 32 ||
      !std::all_of(result.authority_id.begin(), result.authority_id.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
      })) {
    throw Unauthorized();
  }

  // Subtract the whole RPC round trip rather than trusting synchronized monotonic clocks.
  result.expiry = request_started + std::chrono::milliseconds(remaining);
  return result;
}

bool equal_credential(std::string_view expected, std::string_view received) {
  return expected.size() == received.size() &&
         CRYPTO_memcmp(expected.data(), received.data(), expected.size()) == 0;
}

} // namespace

void AuthService::set_auth_authority(std::shared_ptr<AuthAuthority> authority) {
  std::lock_guard lock(mutex_);
  if (!sessions_.empty() || !tickets_.empty() || !launch_grants_.empty()) {
    throw std::logic_error("Authentication authority must be configured before serving players");
  }
  authority_ = std::move(authority);
}

void AuthService::require_world_heartbeat() {
  world_heartbeat_required_ = true;
}

void AuthService::publish_world_status() {
  if (!authority_) {
    throw std::logic_error("World status publication requires a login authority");
  }
  const auto status = world_status();
  const auto reply = authority_->call("world_heartbeat",
                                      {{"world_id", status.world_id}, {"online", status.online}});
  if (!reply.at("accepted").get<bool>()) {
    throw std::runtime_error("Login authority rejected the world heartbeat");
  }
}

AuthService::Session AuthService::authority_session(const std::string &token, bool in_world) {
  if (token.empty() || token.size() > 1024) {
    throw Unauthorized();
  }
  const auto started = now_();
  const auto reply = authority_->call("session", {{"token", token}, {"in_world", in_world}});
  Session session;
  session.account = reply.at("account_id").get<std::int64_t>();
  session.world = reply.at("world_id").get<int>();
  session.launcher = reply.at("launcher").get<bool>();
  session.game_master = reply.at("game_master").get<bool>();
  const auto remaining = reply.at("remaining_ms").get<std::int64_t>();
  const auto maximum =
      std::chrono::duration_cast<std::chrono::milliseconds>(config_.session_ttl).count();
  if (session.account <= 0 || pending_revocations_.contains(session.account) || session.world < 0 ||
      remaining <= 0 || remaining > maximum || (in_world && session.world != config_.world_id)) {
    throw Unauthorized();
  }
  session.expiry = started + std::chrono::milliseconds(remaining);
  if (session.expiry <= now_()) {
    throw Unauthorized();
  }
  if (!reply.at("selected").is_null()) {
    const auto &selected = reply.at("selected");
    ChatIdentity identity{selected.at("world_id").get<int>(),
                          selected.at("character_id").get<std::int64_t>(),
                          selected.at("name").get<std::string>()};
    if (identity.character_id <= 0 || identity.world_id != session.world ||
        identity.name.size() > 64) {
      throw Unauthorized();
    }
    session.selected_character = std::move(identity);
  }
  sessions_.insert_or_assign(token, session);
  return session;
}

Handoff AuthService::register_authority_handoff(const std::string &token, std::int64_t character_id,
                                                const std::string &char_token,
                                                const std::string &packet_key) {
  const auto started = now_();
  const auto reply =
      authority_->call("register_handoff", {{"token", token},
                                            {"character_id", character_id},
                                            {"char_token", char_token},
                                            {"packet_key_hex", hex_encode(packet_key)}});
  auto handoff = decode_handoff(reply, started, config_.ticket_ttl);
  if (handoff.character_id != character_id || handoff.packet_key != packet_key ||
      handoff.world_id != config_.world_id || handoff.expiry <= now_()) {
    throw Unauthorized();
  }
  return handoff;
}

std::optional<Handoff>
AuthService::consume_authority_handoff(std::string_view kind, std::string_view credential,
                                       const std::function<bool(const Handoff &)> &admit,
                                       bool *admission_busy) {
  if (admission_busy) {
    *admission_busy = false;
  }
  const auto started = now_();
  Json arguments{{"kind", kind}, {"credential_hex", hex_encode(std::string(credential))}};
  const auto pending = authority_->call("peek_handoff", arguments);
  if (pending.is_null()) {
    return std::nullopt;
  }
  const auto candidate = decode_handoff(pending, started, config_.ticket_ttl);
  if (candidate.expiry <= now_()) {
    return std::nullopt;
  }
  if (admit && !admit(candidate)) {
    if (admission_busy) {
      *admission_busy = true;
    }
    return std::nullopt;
  }

  // The reservation callback only claims the local character gate. A concurrent
  // reselection cannot substitute another ticket between the peek and consumption.
  arguments["admission_id"] = candidate.admission_id;
  arguments["authority_id"] = candidate.authority_id;
  const auto consumed = authority_->call("consume_handoff", arguments);
  if (consumed.is_null()) {
    return std::nullopt;
  }
  const auto result = decode_handoff(consumed, started, config_.ticket_ttl);
  if (result.admission_id != candidate.admission_id ||
      result.authority_id != candidate.authority_id || result.account_id != candidate.account_id ||
      result.character_id != candidate.character_id || result.world_id != candidate.world_id ||
      result.expiry <= now_()) {
    throw Unauthorized();
  }
  replace_world_admission_locked(result.account_id, result);
  return result;
}

bool AuthService::prepare_world_admission(const Handoff &handoff) {
  if (!authority_) {
    return true;
  }
  const auto result = authority_->call("claim_admission", {{"account_id", handoff.account_id},
                                                           {"admission_id", handoff.admission_id},
                                                           {"authority_id", handoff.authority_id}});
  if (!result.at("accepted").get<bool>() || handoff.expiry <= now_()) {
    return false;
  }

  std::lock_guard lock(admission_mutex_);
  const auto pending = pending_world_admissions_.find(handoff.account_id);
  if (pending == pending_world_admissions_.end() ||
      pending->second.admission_id != handoff.admission_id ||
      pending->second.authority_id != handoff.authority_id) {
    return false;
  }
  prepared_world_admissions_.insert({handoff.authority_id, handoff.admission_id});
  return true;
}

bool AuthService::authority_account_mutation(std::string_view operation, const Json &arguments,
                                             std::int64_t account_id, bool revoke_local) {
  bool accepted{};
  try {
    accepted = authority_->call(operation, arguments).at("accepted").get<bool>();
  } catch (...) {
    // An unavailable reply cannot establish whether revocation committed remotely.
    // Keep this game process closed to the account until an operator retry succeeds.
    std::lock_guard lock(mutex_);
    mark_pending_revocation_locked(account_id);
    throw;
  }
  if (accepted) {
    std::lock_guard lock(mutex_);
    pending_revocations_.erase(account_id);
    if (revoke_local) {
      invalidate_account_sessions_locked(account_id);
    }
  }
  return accepted;
}

Json AuthService::handle_authority_operation(std::string_view operation, const Json &arguments) {
  if (authority_ || !arguments.is_object()) {
    throw std::invalid_argument("This process is not the login authority");
  }
  if (operation == "service_status") {
    return {{"protocol_version", 1},
            {"service", "login"},
            {"world_id", config_.world_id},
            {"database_identity", config_.authority_database_identity},
            {"authority_id", authority_id_}};
  }
  if (operation == "world_heartbeat") {
    const auto world_id = arguments.at("world_id").get<int>();
    const auto online = arguments.at("online").get<unsigned>();
    if (world_id != config_.world_id || online > kMaximumWorldCapacity) {
      throw std::invalid_argument("Invalid world heartbeat");
    }
    std::lock_guard lock(world_heartbeat_mutex_);
    world_heartbeat_ = std::pair(online, now_());
    return {{"accepted", true}};
  }
  if (operation == "set_realm") {
    RealmSettings settings;
    settings.world_name = arguments.at("world_name").get<std::string>();
    settings.site_display_name = arguments.at("site_display_name").get<std::string>();
    settings.capacity = arguments.at("capacity").get<unsigned>();
    settings.label = static_cast<x5::pb::ServerLabel>(arguments.at("label").get<int>());
    settings.maintenance = arguments.at("maintenance").get<bool>();
    settings.char_creation_restricted = arguments.at("char_creation_restricted").get<bool>();
    update_realm_settings(settings);
    return {{"accepted", true}};
  }
  if (operation == "session") {
    std::lock_guard lock(mutex_);
    const auto session = require_session(arguments.at("token").get<std::string>(),
                                         arguments.at("in_world").get<bool>());
    const auto account = accounts_->account_by_id(session.account);
    if (!account || account->banned || pending_revocations_.contains(session.account)) {
      throw Unauthorized();
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(session.expiry - now_()).count();
    Json selected;
    if (session.selected_character) {
      selected = {{"world_id", session.selected_character->world_id},
                  {"character_id", session.selected_character->character_id},
                  {"name", session.selected_character->name}};
    }
    return {{"account_id", session.account}, {"world_id", session.world},
            {"launcher", session.launcher},  {"game_master", session.game_master},
            {"remaining_ms", remaining},     {"selected", std::move(selected)}};
  }
  if (operation == "register_handoff") {
    std::lock_guard lock(mutex_);
    const auto token = arguments.at("token").get<std::string>();
    const auto session = require_session(token, true);
    const auto character_id = arguments.at("character_id").get<std::int64_t>();
    const auto character = characters_->character_summary(character_id);
    const auto char_token = arguments.at("char_token").get<std::string>();
    const auto packet_key = hex_decode(arguments.at("packet_key_hex").get<std::string>());
    if (!character || character->account_id != session.account ||
        character->world_id != session.world || char_token.size() != kWorldTokenBytes ||
        packet_key.size() != kPacketKeyBytes ||
        !std::all_of(char_token.begin(), char_token.end(), [](char byte) {
          return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
        })) {
      throw Unauthorized();
    }

    if (last_world_admission_id_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("World admission identifiers exhausted");
    }
    std::erase_if(tickets_, [&](const auto &entry) {
      return entry.second.account_id == session.account;
    });
    Handoff handoff{session.account, character_id, session.world, packet_key,
                    now_() + config_.ticket_ttl};
    handoff.game_master = session.game_master;
    handoff.authority_id = authority_id_;
    handoff.admission_id = ++last_world_admission_id_;
    if (handoff.admission_id == 0) {
      throw std::overflow_error("World admission identifiers exhausted");
    }
    replace_world_admission_locked(session.account, handoff);
    tickets_.insert_or_assign(char_token, handoff);
    sessions_.at(token).selected_character =
        ChatIdentity{session.world, character_id, character->name};
    return encode_handoff(handoff, now_());
  }
  if (operation == "peek_handoff" || operation == "consume_handoff") {
    const auto kind = arguments.at("kind").get<std::string>();
    const auto credential = hex_decode(arguments.at("credential_hex").get<std::string>());
    if ((kind != "packet_key" && kind != "token") ||
        credential.size() != (kind == "packet_key" ? kPacketKeyBytes : kWorldTokenBytes)) {
      throw Unauthorized();
    }
    std::lock_guard lock(mutex_);
    prune_expired();
    const auto found = std::find_if(tickets_.begin(), tickets_.end(), [&](const auto &entry) {
      return equal_credential(kind == "packet_key" ? std::string_view(entry.second.packet_key)
                                                   : std::string_view(entry.first),
                              credential);
    });
    if (found == tickets_.end()) {
      return nullptr;
    }
    const auto response = encode_handoff(found->second, now_());
    if (operation == "consume_handoff") {
      if (found->second.admission_id != arguments.at("admission_id").get<std::uint64_t>() ||
          found->second.authority_id != arguments.at("authority_id").get<std::string>()) {
        return nullptr;
      }
      {
        std::lock_guard admission_lock(admission_mutex_);
        consumed_authority_handoffs_.insert(
            {found->second.authority_id, found->second.admission_id});
      }
      tickets_.erase(found);
    }
    return response;
  }
  if (operation == "claim_admission") {
    if (arguments.at("authority_id").get<std::string>() != authority_id_) {
      return {{"accepted", false}};
    }
    Handoff candidate;
    candidate.account_id = arguments.at("account_id").get<std::int64_t>();
    candidate.admission_id = arguments.at("admission_id").get<std::uint64_t>();
    candidate.authority_id = authority_id_;
    {
      std::lock_guard lock(admission_mutex_);
      const auto pending = pending_world_admissions_.find(candidate.account_id);
      if (pending == pending_world_admissions_.end() ||
          pending->second.admission_id != candidate.admission_id ||
          !consumed_authority_handoffs_.contains(
              {candidate.authority_id, candidate.admission_id})) {
        return {{"accepted", false}};
      }
      candidate.expiry = pending->second.expiry;
    }
    return {{"accepted", claim_world_admission(candidate)}};
  }
  if (operation == "logout") {
    logout(arguments.at("token").get<std::string>());
    return {{"accepted", true}};
  }
  if (operation == "enter") {
    x5::pb::authwebsocket::WorldEnterReq request;
    parse_json(arguments.at("request"), request);
    return {{"response", message_json(enter(arguments.at("token").get<std::string>(), request))}};
  }
  if (operation == "revoke_account") {
    revoke_account_sessions(arguments.at("account_id").get<std::int64_t>());
    return {{"accepted", true}};
  }
  if (operation == "quarantine_account") {
    quarantine_account_sessions(arguments.at("account_id").get<std::int64_t>());
    return {{"accepted", true}};
  }
  if (operation == "set_password") {
    return {{"accepted", set_account_password(arguments.at("account_id").get<std::int64_t>(),
                                              arguments.at("password").get<std::string>())}};
  }
  if (operation == "set_banned") {
    return {{"accepted", set_account_banned(arguments.at("account_id").get<std::int64_t>(),
                                            arguments.at("banned").get<bool>())}};
  }
  if (operation == "create_account") {
    return {{"accepted", create_account(arguments.at("name").get<std::string>(),
                                        arguments.at("password").get<std::string>())}};
  }
  if (operation == "set_role") {
    const auto role = arguments.at("role").get<int>();
    if (role != static_cast<int>(AccountRole::player) &&
        role != static_cast<int>(AccountRole::game_master)) {
      throw std::invalid_argument("Invalid account role");
    }
    return {{"accepted", set_account_role(arguments.at("name").get<std::string>(),
                                          static_cast<AccountRole>(role))}};
  }
  if (operation == "login") {
    bool credential_mismatch{};
    const auto response =
        login(arguments.at("name").get<std::string>(), arguments.at("password").get<std::string>(),
              arguments.at("launcher").get<bool>(), &credential_mismatch);
    return {{"response", message_json(response)}, {"credential_mismatch", credential_mismatch}};
  }
  if (operation == "issue_grant") {
    const auto grant = issue_launch_grant(arguments.at("token").get<std::string>());
    return {{"grant", grant ? Json(*grant) : Json(nullptr)}};
  }
  if (operation == "valid_grant") {
    return {{"accepted", valid_launch_grant(arguments.at("grant").get<std::string>())}};
  }
  if (operation == "consume_grant" || operation == "reconnect_grant" ||
      operation == "rotate_session") {
    const auto value = arguments.at("credential").get<std::string>();
    std::optional<x5::pa2c::AuthChallengeRes> result;
    if (operation == "consume_grant") {
      result = consume_launch_grant(value);
    } else if (operation == "reconnect_grant") {
      result = reconnect_launch_grant(value);
    } else {
      result = rotate_session(value);
    }
    return result ? Json{{"response", message_json(*result)}} : Json(nullptr);
  }
  if (operation == "bind_session") {
    return {{"accepted", bind_local_session(arguments.at("valid_token").get<std::string>(),
                                            arguments.at("presented_token").get<std::string>())}};
  }
  if (operation == "discord_login") {
    return {
        {"response", message_json(login_discord(arguments.at("discord_id").get<std::string>()))}};
  }
  if (operation == "discord_link") {
    return {{"accepted", link_discord(arguments.at("token").get<std::string>(),
                                      arguments.at("discord_id").get<std::string>())}};
  }
  if (operation == "session_counts") {
    Json counts = Json::array();
    for (const auto &[account_id, count] : lobby_session_counts()) {
      counts.push_back({{"account_id", account_id}, {"count", count}});
    }
    return {{"counts", std::move(counts)}};
  }
  throw std::invalid_argument("Unknown authentication authority operation");
}

} // namespace chronicles
