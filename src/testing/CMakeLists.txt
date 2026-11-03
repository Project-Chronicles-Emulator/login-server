chronicles_add_module(testing
  DEPENDS persistence protocol
  LIBRARIES GTest::gtest spdlog::spdlog
)
