cmake_minimum_required(VERSION 3.28)

project(JournalSeed VERSION 0.1.0 LANGUAGES CXX)

option(JOURNALSEED_BUILD_SERVER "Build the Drogon HTTP server" ON)
option(JOURNALSEED_BUILD_TESTS "Build unit tests" ON)
option(JOURNALSEED_BUILD_BENCHMARKS "Build microbenchmarks" OFF)
option(JOURNALSEED_ENABLE_LTO "Enable interprocedural optimization" OFF)
option(JOURNALSEED_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectWarnings)
include(Sanitizers)

find_package(glaze CONFIG REQUIRED)
find_package(Lua CONFIG REQUIRED)
find_package(mpdecimal CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)

add_subdirectory(backend)

include(CTest)
if(BUILD_TESTING AND JOURNALSEED_BUILD_TESTS)
  add_subdirectory(backend/tests)
endif()

if(JOURNALSEED_BUILD_BENCHMARKS)
  add_subdirectory(backend/benchmarks)
endif()
