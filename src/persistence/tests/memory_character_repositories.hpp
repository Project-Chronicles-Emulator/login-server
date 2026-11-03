#pragma once

// One in-memory database with every persistence repository built over it, for tests that
// previously constructed a combined in-memory store. Members are created in the constructor
// and never reseated; a test may replace one with a fault-injecting repository before
// handing character_repositories() to a service.

#include "persistence/character_repositories.hpp"
#include "persistence/memory_repositories.hpp"

#include <memory>

namespace chronicles {

struct MemoryRepositories {
  std::shared_ptr<MemoryDatabase> database = std::make_shared<MemoryDatabase>();
  std::shared_ptr<MemoryAccountRepository> accounts;
  std::shared_ptr<MemoryCharacterRepository> characters;
  std::shared_ptr<MemoryCharacterStateRepository> character_state;

  MemoryRepositories();

  CharacterRepositories character_repositories() const;
};

inline MemoryRepositories::MemoryRepositories()
    : accounts(std::make_shared<MemoryAccountRepository>(database)),
      characters(std::make_shared<MemoryCharacterRepository>(database)),
      character_state(std::make_shared<MemoryCharacterStateRepository>(database)) {
}

inline CharacterRepositories MemoryRepositories::character_repositories() const {
  return {accounts, characters, character_state};
}

} // namespace chronicles
