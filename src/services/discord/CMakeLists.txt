# services/discord module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(services/discord
  SOURCES
    discord_http.cpp
    discord_oauth.cpp
  DEPENDS
    auth
    distribution
  LIBRARIES
    $<$<PLATFORM_ID:Windows>:winhttp>
    OpenSSL::Crypto
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(services/discord
  NAME chronicles-discord-tests
  SOURCES
    tests/discord_oauth_tests.cpp
  MODULES
    testing
)
