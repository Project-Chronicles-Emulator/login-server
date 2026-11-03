cmake_minimum_required(VERSION 3.25)
project(ChroniclesLoginServer VERSION 0.2.6 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Every executable lands in out/build/windows/bin/<Config>/ so scripts have one place to look;
# multi-config generators append the configuration folder. The WebView2 applications
# (launcher editions and admin) override this with their own subfolders because each keeps a
# web/ folder and launcher-config.json beside its executable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Several translation units (the world session, combat, generated messages and their tests)
# exceed the default COFF section limit; /bigobj only raises that limit.
if(MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/bigobj>)
endif()
include(CTest)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Protobuf CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS asio beast)
find_package(OpenSSL REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
if(BUILD_TESTING)
  find_package(GTest CONFIG REQUIRED)
endif()
if(WIN32)
  enable_language(RC)
endif()
include(cmake/ChroniclesResources.cmake)
include(cmake/ChroniclesPrecompiledHeaders.cmake)
include(cmake/ChroniclesModules.cmake)
add_subdirectory(src)
