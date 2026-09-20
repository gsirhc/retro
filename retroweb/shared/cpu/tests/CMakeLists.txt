cmake_minimum_required(VERSION 3.14)
project(z80_tests CXX)

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

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(cpu_z80_test cpu_z80_test.cpp ${CORE}/cpu_z80.cpp)
target_include_directories(cpu_z80_test PRIVATE ${CORE})
target_link_libraries(cpu_z80_test PRIVATE GTest::gtest_main)
target_compile_options(cpu_z80_test PRIVATE -Wall -Wextra)

include(GoogleTest)
gtest_discover_tests(cpu_z80_test)
