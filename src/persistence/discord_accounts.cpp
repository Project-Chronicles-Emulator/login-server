#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"

#include <algorithm>
#include <pqxx/pqxx>

namespace chronicles {

void validate_discord_id(const std::string &id) {
  if (id.size() < 17 || id.size() > 20 || id.front() == '0' ||
      !std::all_of(id.begin(), id.end(), [](char value) { return value >= '0' && value <= '9'; })) {
    throw std::invalid_argument("Invalid verified Discord identity");
  }
}

std::optional<Account> MemoryAccountRepository::discord_account(const std::string &discord_id) {
  validate_discord_id(discord_id);
  std::scoped_lock lock(database_->mutex);
  const auto identity = database_->discord_accounts.find(discord_id);
  if (identity == database_->discord_accounts.end()) {
    return std::nullopt;
  }
  const auto account = std::ranges::find(database_->accounts, identity->second, &Account::id);
  return account == database_->accounts.end() ? std::nullopt
                                                : std::optional<Account>(*account);
}

Account MemoryAccountRepository::create_discord_account(const std::string &discord_id,
                                                          Account             candidate) {
  validate_discord_id(discord_id);
  std::scoped_lock lock(database_->mutex);
  if (const auto existing = database_->discord_accounts.find(discord_id);
      existing != database_->discord_accounts.end()) {
    const auto account = std::ranges::find(database_->accounts, existing->second, &Account::id);
    if (account == database_->accounts.end()) {
      throw std::runtime_error("Linked Discord account is missing");
    }
    return *account;
  }
  if (std::ranges::find(database_->accounts, candidate.name, &Account::name) !=
      database_->accounts.end()) {
    throw std::runtime_error("Generated Discord account name is unavailable");
  }
  candidate.id = static_cast<std::int64_t>(database_->accounts.size()) + 1;
  candidate.role = AccountRole::player;
  database_->accounts.push_back(candidate);
  database_->discord_accounts.emplace(discord_id, candidate.id);
  return candidate;
}

bool MemoryAccountRepository::link_discord_account(const std::string &discord_id,
                                                     std::int64_t        account_id) {
  validate_discord_id(discord_id);
  std::scoped_lock lock(database_->mutex);
  if (const auto existing = database_->discord_accounts.find(discord_id);
      existing != database_->discord_accounts.end()) {
    return existing->second == account_id;
  }
  if (std::ranges::find(database_->accounts, account_id, &Account::id) ==
          database_->accounts.end() ||
      std::ranges::any_of(database_->discord_accounts,
                          [account_id](const auto &entry) { return entry.second == account_id; })) {
    return false;
  }
  database_->discord_accounts.emplace(discord_id, account_id);
  return true;
}

std::optional<Account> PostgresAccountRepository::discord_account(const std::string &discord_id) {
  validate_discord_id(discord_id);
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows =
      transaction.exec("SELECT account_id FROM emulator_discord_accounts WHERE discord_user_id=$1",
                       pqxx::params{discord_id});
  if (rows.empty()) {
    return std::nullopt;
  }
  return account_by_id(rows.front()[0].as<std::int64_t>());
}

Account PostgresAccountRepository::create_discord_account(const std::string &discord_id,
                                                          Account             candidate) {
  validate_discord_id(discord_id);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  // Serialize first login for this identity across processes; both rows commit together.
  transaction.exec("SELECT pg_advisory_xact_lock(hashtextextended($1,0))",
                   pqxx::params{"chronicles-discord:" + discord_id});
  const auto existing =
      transaction.exec("SELECT account_id FROM emulator_discord_accounts WHERE discord_user_id=$1",
                       pqxx::params{discord_id});
  std::int64_t account_id{};
  if (existing.empty()) {
    const auto rows = transaction.exec(
        "INSERT INTO "
        "emulator_accounts(account_name,password_salt,password_hash,password_iterations) "
        "VALUES ($1,$2,$3,$4) RETURNING id",
        pqxx::params{candidate.name, candidate.salt, candidate.hash, candidate.iterations});
    account_id = rows.front()[0].as<std::int64_t>();
    transaction.exec(
        "INSERT INTO emulator_discord_accounts(discord_user_id,account_id) VALUES ($1,$2)",
        pqxx::params{discord_id, account_id});
  } else {
    account_id = existing.front()[0].as<std::int64_t>();
  }
  transaction.commit();
  const auto account = account_by_id(account_id);
  if (!account) {
    throw std::runtime_error("Linked Discord account is missing");
  }
  return *account;
}

bool PostgresAccountRepository::link_discord_account(const std::string &discord_id,
                                                     std::int64_t        account_id) {
  validate_discord_id(discord_id);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  transaction.exec("INSERT INTO emulator_discord_accounts(discord_user_id,account_id) "
                   "SELECT $1,id FROM emulator_accounts WHERE id=$2 AND NOT banned "
                   "ON CONFLICT DO NOTHING",
                   pqxx::params{discord_id, account_id});
  const auto rows =
      transaction.exec("SELECT account_id FROM emulator_discord_accounts WHERE discord_user_id=$1",
                       pqxx::params{discord_id});
  const bool linked = !rows.empty() && rows.front()[0].as<std::int64_t>() == account_id;
  transaction.commit();
  return linked;
}

} // namespace chronicles
