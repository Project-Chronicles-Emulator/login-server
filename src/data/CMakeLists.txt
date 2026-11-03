# data module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(data
  SOURCES
    catalog_requirements.cpp
    client_catalog.cpp
    companion_catalog.cpp
    shop_catalog.cpp
    shop_conditions.cpp
    trade_catalog.cpp
  DEPENDS
    common
  LIBRARIES
    OpenSSL::Crypto
    chronicles-messages
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(data
  SOURCES
    tests/client_catalog_tests.cpp
  LIBRARIES
    OpenSSL::Crypto
)
