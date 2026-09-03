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

# -DCOVERAGE=ON instruments just our test binaries (not googletest) for
# source-based coverage. `make -C Emulator8080 coverage` drives the flow.
option(COVERAGE "instrument the unit-test binaries for llvm-cov" OFF)
function(add_unit_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${name} PRIVATE GTest::gtest_main)
  target_compile_options(${name} PRIVATE -Wall -Wextra)
  if(COVERAGE)
    target_compile_options(${name} PRIVATE -O0 -g -fprofile-instr-generate -fcoverage-mapping)
    target_link_options(${name} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
  endif()
endfunction()

add_unit_test(arithmetic_test arithmetic_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../i8080.cpp)
add_unit_test(disk88_test     disk88_test.cpp     ${CMAKE_CURRENT_SOURCE_DIR}/../disk88.cpp)
add_unit_test(cassette_test   cassette_test.cpp   ${CMAKE_CURRENT_SOURCE_DIR}/../cassette.cpp)

include(GoogleTest)
gtest_discover_tests(arithmetic_test)
gtest_discover_tests(disk88_test)
gtest_discover_tests(cassette_test)
