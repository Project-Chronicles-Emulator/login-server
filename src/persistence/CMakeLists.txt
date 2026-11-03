# persistence module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(persistence
  SOURCES
    admin_storage.cpp
    character_repositories.cpp
    character_summary_storage.cpp
    company_permit_storage.cpp
    discord_accounts.cpp
    loot_claim_storage.cpp
    memory_database.cpp
    quest_service_effects.cpp
    quest_service_effect_codec.cpp
    quest_service_effect_guards.cpp
    quest_service_postgres.cpp
    skill_item_use.cpp
    storage.cpp
    world_storage.cpp
  DEPENDS
    common
    data
  LIBRARIES
    libpqxx::pqxx
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(persistence
  SOURCES
    tests/storage_tests.cpp
  MODULES
    data
    testing
  LIBRARIES
    libpqxx::pqxx
)
