cmake_minimum_required(VERSION 3.14)
project(ibmpcat_tests CXX)

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
add_unit_test(cpu80286_test cpu80286_test.cpp ${CORE}/cpu80286.cpp)
add_unit_test(pic8259_test  pic8259_test.cpp  ${CORE}/pic8259.cpp)
add_unit_test(pit8253_test  pit8253_test.cpp  ${CORE}/pit8253.cpp)
add_unit_test(i8042_test    i8042_test.cpp    ${CORE}/i8042.cpp)
add_unit_test(cmos_rtc_test cmos_rtc_test.cpp ${CORE}/cmos_rtc.cpp)
add_unit_test(dma8237_test  dma8237_test.cpp  ${CORE}/dma8237.cpp)
add_unit_test(chipset_test  chipset_test.cpp
  ${CORE}/chipset.cpp ${CORE}/pic8259.cpp ${CORE}/pit8253.cpp
  ${CORE}/i8042.cpp ${CORE}/cmos_rtc.cpp ${CORE}/dma8237.cpp ${CORE}/fdc765.cpp ${CORE}/ega.cpp ${CORE}/wd1003.cpp)
add_unit_test(machine_test  machine_test.cpp
  ${CORE}/machine.cpp ${CORE}/chipset.cpp ${CORE}/cpu80286.cpp
  ${CORE}/pic8259.cpp ${CORE}/pit8253.cpp ${CORE}/i8042.cpp
  ${CORE}/cmos_rtc.cpp ${CORE}/dma8237.cpp ${CORE}/fdc765.cpp ${CORE}/ega.cpp ${CORE}/wd1003.cpp)
add_unit_test(fdc765_test   fdc765_test.cpp  ${CORE}/fdc765.cpp)
add_unit_test(ega_test      ega_test.cpp     ${CORE}/ega.cpp)
add_unit_test(wd1003_test   wd1003_test.cpp  ${CORE}/wd1003.cpp)

include(GoogleTest)
gtest_discover_tests(cpu80286_test)
gtest_discover_tests(pic8259_test)
gtest_discover_tests(pit8253_test)
gtest_discover_tests(i8042_test)
gtest_discover_tests(cmos_rtc_test)
gtest_discover_tests(dma8237_test)
gtest_discover_tests(chipset_test)
gtest_discover_tests(machine_test)
gtest_discover_tests(fdc765_test)
gtest_discover_tests(ega_test)
gtest_discover_tests(wd1003_test)
