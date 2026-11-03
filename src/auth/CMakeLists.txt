# auth module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(auth
  SOURCES
    auth.cpp
    auth_authority.cpp
    auth_rpc.cpp
    auth_wire.cpp
    auth_discord.cpp
    launcher_roster.cpp
    native_auth.cpp
  DEPENDS
    data
    distribution
    persistence
  LIBRARIES
    OpenSSL::Crypto
    Boost::asio
    chronicles-messages
    nlohmann_json::nlohmann_json
    protobuf::libprotobuf
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(auth
  NAME chronicles-roster-tests
  SOURCES
    tests/auth_authority_tests.cpp
    tests/auth_split_tests.cpp
    tests/auth_service_tests.cpp
    tests/launcher_roster_tests.cpp
    tests/native_auth_tests.cpp
  MODULES
    data
    testing
  LIBRARIES
    OpenSSL::Crypto
)

# Session/grant regression checks kept runnable under their historical CTest prefix.
chronicles_add_module_tests(auth
  NAME chronicles-auth-flow-tests
  TEST_PREFIX AuthFlow.
  SOURCES
    tests/native_auth_tests.cpp
  MODULES
    testing
)
