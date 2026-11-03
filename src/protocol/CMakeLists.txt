# protocol module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(protocol
  SOURCES
    protocol.cpp
  LIBRARIES
    nlohmann_json::nlohmann_json
    protobuf::libprotobuf
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(protocol
  SOURCES
    tests/encode_once_tests.cpp
    tests/protocol_tests.cpp
  MODULES
    auth
)
