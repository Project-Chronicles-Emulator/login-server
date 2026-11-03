# common module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(common
  SOURCES
    currency.cpp
    world_metrics.cpp
  LIBRARIES
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(common
  NAME chronicles-currency-tests
  SOURCES
    tests/currency_tests.cpp
    tests/world_metrics_tests.cpp
  MODULES
    net
  LIBRARIES
    nlohmann_json::nlohmann_json
)
