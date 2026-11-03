#include "persistence/character_repositories.hpp"

#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"

#include <stdexcept>
#include <utility>

namespace chronicles {

CharacterRepositories
make_memory_character_repositories(std::shared_ptr<MemoryDatabase> database) {
  if (!database) {
    throw std::invalid_argument("Memory character repositories require a database");
  }

  CharacterRepositories repositories;
  repositories.accounts = std::make_shared<MemoryAccountRepository>(database);
  repositories.characters = std::make_shared<MemoryCharacterRepository>(database);
  repositories.character_state =
      std::make_shared<MemoryCharacterStateRepository>(std::move(database));
  return repositories;
}

CharacterRepositories make_postgres_character_repositories(const std::string &connection) {
  CharacterRepositories repositories;
  repositories.accounts = std::make_shared<PostgresAccountRepository>(connection);
  repositories.characters = std::make_shared<PostgresCharacterRepository>(connection);
  repositories.character_state =
      std::make_shared<PostgresCharacterStateRepository>(connection);
  return repositories;
}

} // namespace chronicles
