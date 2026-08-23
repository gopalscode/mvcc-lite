cmake_minimum_required(VERSION 3.16)
project(mvcc_lite CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Warnings-as-a-safety-net: catch mistakes at compile time rather than at
# 2am during a stress test.
add_compile_options(-Wall -Wextra)

# Optional sanitizer builds:
#   cmake -DMVCC_SANITIZE=thread ..    (catch data races)
#   cmake -DMVCC_SANITIZE=address ..   (catch memory bugs)
set(MVCC_SANITIZE "" CACHE STRING "Sanitizer to build with: thread, address, or empty")
if(MVCC_SANITIZE)
  add_compile_options(-fsanitize=${MVCC_SANITIZE} -g)
  add_link_options(-fsanitize=${MVCC_SANITIZE})
endif()

add_library(mvcc_core
  src/kv_store.cpp
  src/version_store.cpp
  src/transaction.cpp
  src/gc.cpp
)
target_include_directories(mvcc_core PUBLIC include)
target_link_libraries(mvcc_core PUBLIC pthread)

option(MVCC_BUILD_TESTS "Build unit and stress tests" ON)
if(MVCC_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  # Windows-only setting; harmless elsewhere, keeps the config portable.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(kv_store_tests tests/test_kv_store.cpp)
  target_link_libraries(kv_store_tests PRIVATE mvcc_core GTest::gtest_main)

  add_executable(kv_store_stress_tests tests/stress_test_kv_store.cpp)
  target_link_libraries(kv_store_stress_tests PRIVATE mvcc_core GTest::gtest_main)

  add_executable(version_store_tests tests/test_version_store.cpp)
  target_link_libraries(version_store_tests PRIVATE mvcc_core GTest::gtest_main)

  add_executable(version_store_stress_tests tests/stress_test_version_store.cpp)
  target_link_libraries(version_store_stress_tests PRIVATE mvcc_core GTest::gtest_main)

  add_executable(transaction_tests tests/test_transaction.cpp)
  target_link_libraries(transaction_tests PRIVATE mvcc_core GTest::gtest_main)

  add_executable(transaction_stress_tests tests/stress_test_transaction.cpp)
  target_link_libraries(transaction_stress_tests PRIVATE mvcc_core GTest::gtest_main)

  add_executable(gc_tests tests/test_gc.cpp)
  target_link_libraries(gc_tests PRIVATE mvcc_core GTest::gtest_main)

  add_executable(gc_stress_tests tests/stress_test_gc.cpp)
  target_link_libraries(gc_stress_tests PRIVATE mvcc_core GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(kv_store_tests)
  gtest_discover_tests(kv_store_stress_tests)
  gtest_discover_tests(version_store_tests)
  gtest_discover_tests(version_store_stress_tests)
  gtest_discover_tests(transaction_tests)
  gtest_discover_tests(transaction_stress_tests)
  gtest_discover_tests(gc_tests)
  gtest_discover_tests(gc_stress_tests)
endif()

option(MVCC_BUILD_BENCHMARKS "Build the Phase 6 benchmark (mvcc_bench)" ON)
if(MVCC_BUILD_BENCHMARKS)
  add_executable(mvcc_bench bench/bench_mvcc_vs_kv.cpp)
  target_link_libraries(mvcc_bench PRIVATE mvcc_core)
endif()
