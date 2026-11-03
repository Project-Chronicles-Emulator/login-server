# services/portal_storage module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(services/portal_storage
  SOURCES
    portal_store.cpp
  DEPENDS
    services/portal
  LIBRARIES
    libpqxx::pqxx
    nlohmann_json::nlohmann_json
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(services/portal_storage
  NAME chronicles-portal-tests
  SOURCES
    tests/portal_tests.cpp
  MODULES
    services/portal
)
