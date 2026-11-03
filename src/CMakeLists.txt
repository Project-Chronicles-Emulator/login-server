set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(DESCRIPTORS "${PROJECT_SOURCE_DIR}/protocol/descriptors.normalized.pb")

execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/generate_protos.py"
    --descriptor "${DESCRIPTORS}" --list
  OUTPUT_VARIABLE PROTO_FILES
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)

set(GENERATED_SOURCES)
set(GENERATED_HEADERS)
foreach(PROTO IN LISTS PROTO_FILES)
  string(REGEX REPLACE "\\.proto$" ".pb.cc" CC "${PROTO}")
  string(REGEX REPLACE "\\.proto$" ".pb.h" HH "${PROTO}")
  list(APPEND GENERATED_SOURCES "${GENERATED_DIR}/${CC}")
  list(APPEND GENERATED_HEADERS "${GENERATED_DIR}/${HH}")
endforeach()

add_custom_command(
  OUTPUT ${GENERATED_SOURCES} ${GENERATED_HEADERS}
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/generate_protos.py"
    --descriptor "${DESCRIPTORS}"
    --protoc "$<TARGET_FILE:protobuf::protoc>"
    --output "${GENERATED_DIR}"
  DEPENDS
    "${DESCRIPTORS}"
    "${PROJECT_SOURCE_DIR}/tools/generate_protos.py"
    protobuf::protoc
  VERBATIM
)

add_library(chronicles-messages STATIC ${GENERATED_SOURCES} ${GENERATED_HEADERS})
target_include_directories(chronicles-messages PUBLIC "${GENERATED_DIR}")
target_link_libraries(chronicles-messages PUBLIC protobuf::libprotobuf)
if(MSVC)
  target_compile_options(chronicles-messages PRIVATE /wd4251 /wd4996)
endif()


add_subdirectory(common)
add_subdirectory(distribution)
add_subdirectory(protocol)
add_subdirectory(data)
add_subdirectory(persistence)
add_subdirectory(auth)
add_subdirectory(services/discord)
add_subdirectory(services/news)
add_subdirectory(services/portal)
add_subdirectory(services/portal_storage)
add_subdirectory(services/login)
add_subdirectory(net)
if(BUILD_TESTING)
  add_subdirectory(testing)
endif()
add_subdirectory(apps)
install(TARGETS chronicles-login-server RUNTIME DESTINATION . COMPONENT Login)
install(DIRECTORY "$<TARGET_FILE_DIR:chronicles-login-server>/" DESTINATION . COMPONENT Login
  FILES_MATCHING PATTERN "*.dll" PATTERN "gtest*" EXCLUDE)
install(FILES "${PROJECT_SOURCE_DIR}/config/services/login.example.json" DESTINATION config/services COMPONENT Login)
install(FILES "${PROJECT_SOURCE_DIR}/migrations/001_accounts_characters.sql"
  DESTINATION migrations COMPONENT Login)
install(FILES "${PROJECT_SOURCE_DIR}/tools/install_database.ps1"
  DESTINATION tools COMPONENT Login)
install(FILES "${PROJECT_SOURCE_DIR}/SETUP.md"
  DESTINATION . RENAME README.md COMPONENT Login)
