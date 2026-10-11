cmake_minimum_required(VERSION 3.20)
project(VertexLadder VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(VL_BUILD_TESTS "Build unit tests" ON)
option(VL_BUILD_BENCH "Build benchmarks" ON)
option(VL_BUILD_GATEWAY "Build the FIX 4.4 gateway (needs Boost.Asio)" ON)
option(VL_NATIVE "Tune for this CPU (-march/-mcpu=native)" OFF)
set(VL_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(VL_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-march=native VL_HAS_MARCH_NATIVE)
  if(VL_HAS_MARCH_NATIVE)
    add_compile_options(-march=native)
  else()
    add_compile_options(-mcpu=native)
  endif()
endif()
if(VL_SANITIZE)
  add_compile_options(-fsanitize=${VL_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${VL_SANITIZE})
endif()

find_package(Threads REQUIRED)

# Header-only core: order book, hash map, SPSC ring, ITCH decoder.
add_library(vl_core INTERFACE)
target_include_directories(vl_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vl_core INTERFACE Threads::Threads)

# FIX 4.4 codec (no networking, so it builds and tests without Boost).
add_library(vl_fix STATIC src/fix/fix_codec.cpp)
target_link_libraries(vl_fix PUBLIC vl_core)

if(VL_BUILD_GATEWAY)
  find_package(Boost 1.74 REQUIRED CONFIG)
  add_library(vl_gateway STATIC src/gateway/fix_gateway.cpp)
  target_link_libraries(vl_gateway PUBLIC vl_fix Boost::headers)

  add_executable(vl_server src/apps/vl_server.cpp)
  target_link_libraries(vl_server PRIVATE vl_gateway)

  add_executable(vl_fix_client src/apps/vl_fix_client.cpp)
  target_include_directories(vl_fix_client PRIVATE tests)
  target_link_libraries(vl_fix_client PRIVATE vl_fix Boost::headers)
endif()

if(VL_BUILD_BENCH)
  add_executable(bench_engine bench/bench_engine.cpp)
  target_link_libraries(bench_engine PRIVATE vl_core)
  add_executable(bench_hop bench/bench_hop.cpp)
  target_link_libraries(bench_hop PRIVATE vl_core)
  add_executable(bench_tcp_baseline bench/bench_tcp_baseline.cpp)
  target_link_libraries(bench_tcp_baseline PRIVATE vl_core)

  find_package(ZLIB)
  if(ZLIB_FOUND)
    add_executable(bench_itch bench/bench_itch.cpp)
    target_link_libraries(bench_itch PRIVATE vl_core ZLIB::ZLIB)
  endif()
endif()

if(VL_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  set(VL_TESTS
    tests/test_order_book.cpp
    tests/test_differential.cpp
    tests/test_flat_hash_map.cpp
    tests/test_spsc_ring.cpp
    tests/test_fix_codec.cpp
    tests/test_itch.cpp)
  add_executable(vl_tests ${VL_TESTS})
  if(VL_BUILD_GATEWAY)
    target_sources(vl_tests PRIVATE tests/test_gateway.cpp)
    target_link_libraries(vl_tests PRIVATE vl_gateway GTest::gtest_main)
  else()
    target_link_libraries(vl_tests PRIVATE vl_fix GTest::gtest_main)
  endif()

  include(GoogleTest)
  gtest_discover_tests(vl_tests DISCOVERY_TIMEOUT 60)
endif()
