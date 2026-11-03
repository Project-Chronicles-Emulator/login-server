#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <utility>

namespace chronicles {
namespace {

constexpr const char *kCharacterSummaryColumns =
    "SELECT id,account_id,world_id,lobby_slot,class_id,level,name FROM emulator_characters ";

CharacterSummary summarize(const Character &character) {
  CharacterSummary summary;
  summary.id = character.id;
  summary.account_id = character.account_id;
  summary.world_id = character.world_id;
  summary.slot = character.slot;
  summary.class_id = character.class_id;
  summary.level = character.level;
  summary.name = character.name;
  return summary;
}

CharacterSummary summary_from_row(const pqxx::row &row) {
  CharacterSummary summary;
  summary.id = row["id"].as<std::int64_t>();
  summary.account_id = row["account_id"].as<std::int64_t>();
  summary.world_id = row["world_id"].as<int>();
  summary.slot = row["lobby_slot"].as<int>();
  summary.class_id = row["class_id"].as<int>();
  summary.level = row["level"].as<int>();
  summary.name = row["name"].as<std::string>();
  return summary;
}

std::optional<CharacterSummary> first_summary(const pqxx::result &rows) {
  if (rows.empty()) {
    return std::nullopt;
  }
  return summary_from_row(rows.front());
}

} // namespace

std::optional<CharacterSummary>
MemoryCharacterRepository::character_summary(std::int64_t character_id) {
  std::scoped_lock lock(database_->mutex);
  for (const auto &character : database_->characters) {
    if (character.id == character_id) {
      return summarize(character);
    }
  }
  return std::nullopt;
}

std::optional<CharacterSummary>
MemoryCharacterRepository::character_summary_by_name(int                world_id,
                                                       const std::string &name) {
  std::scoped_lock lock(database_->mutex);
  for (const auto &character : database_->characters) {
    if (character.world_id == world_id && character.name == name) {
      return summarize(character);
    }
  }
  return std::nullopt;
}

std::vector<CharacterSummary>
MemoryCharacterRepository::account_character_summaries(std::int64_t account_id, int world_id) {
  std::scoped_lock lock(database_->mutex);
  std::vector<CharacterSummary> summaries;
  for (const auto &character : database_->characters) {
    if (character.account_id == account_id && character.world_id == world_id) {
      summaries.push_back(summarize(character));
    }
  }

  std::ranges::sort(summaries, {}, &CharacterSummary::slot);
  return summaries;
}

std::optional<CharacterSummary>
PostgresCharacterRepository::character_summary(std::int64_t character_id) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows = transaction.exec(std::string(kCharacterSummaryColumns) + "WHERE id=$1",
                                     pqxx::params{character_id});
  return first_summary(rows);
}

std::optional<CharacterSummary>
PostgresCharacterRepository::character_summary_by_name(int                world_id,
                                                       const std::string &name) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows =
      transaction.exec(std::string(kCharacterSummaryColumns) + "WHERE world_id=$1 AND name=$2",
                       pqxx::params{world_id, name});
  return first_summary(rows);
}

std::vector<CharacterSummary>
PostgresCharacterRepository::account_character_summaries(std::int64_t account_id, int world_id) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows = transaction.exec(std::string(kCharacterSummaryColumns) +
                                         "WHERE account_id=$1 AND world_id=$2 ORDER BY lobby_slot",
                                     pqxx::params{account_id, world_id});

  std::vector<CharacterSummary> summaries;
  summaries.reserve(rows.size());
  for (const auto &row : rows) {
    summaries.push_back(summary_from_row(row));
  }
  return summaries;
}

} // namespace chronicles
