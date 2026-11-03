#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles {

inline constexpr int kDefaultPasswordIterations = 600000;

// Server-side account privilege. Only an operator changes it (chronicles-account role);
// it is never derived from a client claim. The persisted value is the enum integer.
enum class AccountRole {
  player = 0,
  game_master = 1,
};

// Stable operator-facing names: "player" and "gm".
std::string_view account_role_name(AccountRole role);

// Accepts "player", "gm" or "game_master" (ASCII case-insensitive).
std::optional<AccountRole> parse_account_role(std::string_view text);

struct Account {
  std::int64_t id{};
  std::string name;
  std::string salt;
  std::string hash;
  int iterations{kDefaultPasswordIterations};
  AccountRole role{AccountRole::player};
  // Operator-applied suspension (admin console). A banned account cannot log in.
  bool banned{};
};

// Operator console listing row; never derived from or returned to a game client.
struct AccountSummary {
  std::int64_t id{};
  std::string name;
  AccountRole role{AccountRole::player};
  bool banned{};
  int character_count{};
  // UTC ISO-8601 text from PostgreSQL; empty for the memory store.
  std::string created_at;
};

// Throws std::invalid_argument unless the string is a verified Discord snowflake. Shared by
// the Discord account store methods and the AuthService Discord login/link methods.
void validate_discord_id(const std::string &id);

std::string ascii_lower(std::string value);

// Accounts, Discord links, roles, reconnect credentials and operator account changes.
// Implementations: MemoryAccountRepository and PostgresAccountRepository.
class AccountRepository {
public:
  virtual ~AccountRepository() = default;

  virtual std::optional<Account> account(const std::string &name) = 0;

  virtual bool add_account(Account account) = 0;

  virtual std::optional<Account> discord_account(const std::string &discord_id) = 0;
  virtual Account create_discord_account(const std::string &discord_id, Account candidate) = 0;
  virtual bool link_discord_account(const std::string &discord_id, std::int64_t account_id) = 0;

  // Changes an existing account's privilege. Returns false when no account has this name.
  // Sessions keep the role they were issued with; the change applies at the next login.
  virtual bool set_account_role(const std::string &name, AccountRole role) = 0;

  // Operator console operations. Only the authenticated admin API calls these; no
  // game-client request reaches them.
  virtual std::vector<AccountSummary> list_accounts() = 0;

  virtual std::optional<Account> account_by_id(std::int64_t account_id) = 0;

  // Reconnect credentials are stored only as SHA-256 digests. A new launch replaces
  // the account's previous credential. Expiry is an absolute Unix timestamp in seconds.
  virtual bool save_reconnect_credential(std::int64_t account_id, const std::string &digest,
                                         std::int64_t expires_at) = 0;

  virtual std::optional<Account> reconnect_account(const std::string &digest,
                                                   std::int64_t now) = 0;

  virtual void revoke_reconnect_credentials(std::int64_t account_id) = 0;

  // A successful password change atomically revokes that account's reconnect credentials.
  virtual bool set_account_password(std::int64_t account_id, const std::string &salt,
                                    const std::string &hash, int iterations) = 0;

  // A successful ban atomically revokes that account's reconnect credentials.
  virtual bool set_account_banned(std::int64_t account_id, bool banned) = 0;
};

} // namespace chronicles
