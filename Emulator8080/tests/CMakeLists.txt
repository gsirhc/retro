cmake_minimum_required(VERSION 3.14)
project(emu8080_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(arithmetic_test
  arithmetic_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../i8080.cpp
)
target_include_directories(arithmetic_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(arithmetic_test PRIVATE GTest::gtest_main)
target_compile_options(arithmetic_test PRIVATE -Wall -Wextra)

add_executable(disk88_test
  disk88_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../disk88.cpp
)
target_include_directories(disk88_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(disk88_test PRIVATE GTest::gtest_main)
target_compile_options(disk88_test PRIVATE -Wall -Wextra)

add_executable(cassette_test
  cassette_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../cassette.cpp
)
target_include_directories(cassette_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(cassette_test PRIVATE GTest::gtest_main)
target_compile_options(cassette_test PRIVATE -Wall -Wextra)

include(GoogleTest)
gtest_discover_tests(arithmetic_test)
gtest_discover_tests(disk88_test)
gtest_discover_tests(cassette_test)
