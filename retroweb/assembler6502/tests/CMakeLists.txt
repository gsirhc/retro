cmake_minimum_required(VERSION 3.14)
project(cg_oac_6502_tests CXX)

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

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_unit_test(cpu65c02_test    cpu65c02_test.cpp    ${CORE}/cpu65c02.cpp)
add_unit_test(via65c22_test    via65c22_test.cpp    ${CORE}/via65c22.cpp)
add_unit_test(acia65c51_test   acia65c51_test.cpp   ${CORE}/acia65c51.cpp)
add_unit_test(eeprom28c256_test eeprom28c256_test.cpp ${CORE}/eeprom28c256.cpp)
add_unit_test(hd44780_test     hd44780_test.cpp     ${CORE}/hd44780.cpp)
add_unit_test(bus_test         bus_test.cpp
  ${CORE}/bus.cpp ${CORE}/via65c22.cpp ${CORE}/acia65c51.cpp ${CORE}/eeprom28c256.cpp)
add_unit_test(machine_test     machine_test.cpp
  ${CORE}/machine.cpp ${CORE}/bus.cpp ${CORE}/cpu65c02.cpp ${CORE}/via65c22.cpp
  ${CORE}/acia65c51.cpp ${CORE}/eeprom28c256.cpp ${CORE}/hd44780.cpp)
add_unit_test(assembler_test   assembler_test.cpp
  ${CORE}/machine.cpp ${CORE}/bus.cpp ${CORE}/cpu65c02.cpp ${CORE}/via65c22.cpp
  ${CORE}/acia65c51.cpp ${CORE}/eeprom28c256.cpp ${CORE}/hd44780.cpp)
add_unit_test(editor_test      editor_test.cpp
  ${CORE}/machine.cpp ${CORE}/bus.cpp ${CORE}/cpu65c02.cpp ${CORE}/via65c22.cpp
  ${CORE}/acia65c51.cpp ${CORE}/eeprom28c256.cpp ${CORE}/hd44780.cpp)
add_unit_test(load_test        load_test.cpp
  ${CORE}/machine.cpp ${CORE}/bus.cpp ${CORE}/cpu65c02.cpp ${CORE}/via65c22.cpp
  ${CORE}/acia65c51.cpp ${CORE}/eeprom28c256.cpp ${CORE}/hd44780.cpp)

include(GoogleTest)
gtest_discover_tests(cpu65c02_test)
gtest_discover_tests(via65c22_test)
gtest_discover_tests(acia65c51_test)
gtest_discover_tests(eeprom28c256_test)
gtest_discover_tests(hd44780_test)
gtest_discover_tests(bus_test)
gtest_discover_tests(machine_test)
gtest_discover_tests(assembler_test)
gtest_discover_tests(editor_test)
gtest_discover_tests(load_test)
