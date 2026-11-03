#include "auth/auth.hpp"
#include "auth/auth_authority.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace chronicles {

x5::pa2c::AuthChallengeRes AuthService::login_discord(const std::string &verified_discord_id) {
  if (authority_) {
    const auto reply = authority_->call("discord_login", {{"discord_id", verified_discord_id}});
    x5::pa2c::AuthChallengeRes response;
    parse_json(reply.at("response"), response);
    return response;
  }
  validate_discord_id(verified_discord_id);
  auto account = accounts_->discord_account(verified_discord_id);
  if (!account) {
    // Discord usernames and email addresses never select or take over password accounts.
    const auto name = "discord_" + random_hex(8);
    const auto password = random_hex(32);
    auto candidate = make_password_account(name, password);
    account = accounts_->create_discord_account(verified_discord_id, std::move(candidate));
  }
  return create_login_session(*account, true);
}

bool AuthService::link_discord(const std::string &token, const std::string &verified_discord_id) {
  if (authority_) {
    return authority_->call("discord_link", {{"token", token}, {"discord_id", verified_discord_id}})
        .at("accepted").get<bool>();
  }
  validate_discord_id(verified_discord_id);
  std::int64_t account_id{};
  {
    std::lock_guard lock(mutex_);
    const auto session = require_session(token);
    if (!session.launcher) {
      return false;
    }
    account_id = session.account;
  }
  const auto account = accounts_->account_by_id(account_id);
  return account && !account->banned &&
         accounts_->link_discord_account(verified_discord_id, account_id);
}

std::string AuthService::account_name(const std::string &token) {
  std::int64_t account_id{};
  {
    std::lock_guard lock(mutex_);
    account_id = require_session(token).account;
  }
  const auto account = accounts_->account_by_id(account_id);
  if (!account || account->banned) {
    throw Unauthorized();
  }
  return account->name;
}

} // namespace chronicles
