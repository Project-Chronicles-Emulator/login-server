#pragma once

// The three persistence repositories every character-owning service needs (accounts,
// character rows and revisioned character state), built over one shared memory database or
// one PostgreSQL connection string.

#include "persistence/account_repository.hpp"
#include "persistence/character_repository.hpp"
#include "persistence/character_state_repository.hpp"

#include <memory>
#include <string>

namespace chronicles {

struct MemoryDatabase;

struct CharacterRepositories {
  std::shared_ptr<AccountRepository> accounts;
  std::shared_ptr<CharacterRepository> characters;
  std::shared_ptr<CharacterStateRepository> character_state;
};

// Repositories over the shared in-memory tables; callers that register CharacterDeletionHook
// callbacks keep `database` themselves.
CharacterRepositories
make_memory_character_repositories(std::shared_ptr<MemoryDatabase> database);

// Repositories that each open their own pqxx::connection per call.
CharacterRepositories make_postgres_character_repositories(const std::string &connection);

} // namespace chronicles
