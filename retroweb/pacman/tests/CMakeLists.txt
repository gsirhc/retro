cmake_minimum_required(VERSION 3.14)
project(pacman_tests CXX)

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
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(${name} PRIVATE GTest::gtest_main)
  target_compile_options(${name} PRIVATE -Wall -Wextra)
  if(COVERAGE)
    target_compile_options(${name} PRIVATE -O0 -g -fprofile-instr-generate -fcoverage-mapping)
    target_link_options(${name} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
  endif()
endfunction()

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hwtest_roms.h
  COMMAND python3 ${CORE}/roms/hwtest/gen_hwtest.py
          --dir ${CMAKE_CURRENT_BINARY_DIR}/hwtest
          --header ${CMAKE_CURRENT_BINARY_DIR}/hwtest_roms.h
  DEPENDS ${CORE}/roms/hwtest/gen_hwtest.py
  COMMENT "generate Pac-Man hardware self-test ROM"
)
add_custom_target(hwtest_roms DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/hwtest_roms.h)

add_unit_test(cpu_z80_test cpu_z80_test.cpp ${CORE}/cpu_z80.cpp)
add_unit_test(video_wsg_test video_wsg_test.cpp ${CORE}/video.cpp ${CORE}/wsg.cpp)
add_unit_test(machine_test machine_test.cpp
  ${CORE}/machine.cpp ${CORE}/cpu_z80.cpp ${CORE}/video.cpp ${CORE}/wsg.cpp)
add_dependencies(machine_test hwtest_roms)

include(GoogleTest)
gtest_discover_tests(cpu_z80_test)
gtest_discover_tests(video_wsg_test)
gtest_discover_tests(machine_test)
