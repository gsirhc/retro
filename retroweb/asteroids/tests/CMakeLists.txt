cmake_minimum_required(VERSION 3.14)
project(asteroids_tests CXX)

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
set(CPU ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/cpu)
set(ATARI ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/atari)
set(WEB ${CMAKE_CURRENT_SOURCE_DIR}/../web)

# Regenerate hwtest header before compiling smoke.
add_custom_command(
  OUTPUT ${WEB}/hwtest_roms.h
  COMMAND python3 ${CORE}/roms/hwtest/gen_hwtest.py --dir ${WEB}/roms --header ${WEB}/hwtest_roms.h
  DEPENDS ${CORE}/roms/hwtest/gen_hwtest.py
  COMMENT "Generating Asteroids hwtest ROM"
)

set(BOARD
  ${CORE}/machine.cpp
  ${CPU}/cpu_mos6502.cpp
  ${ATARI}/dvg.cpp
)

function(add_unit_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE ${CORE} ${CPU} ${ATARI} ${WEB})
  target_link_libraries(${name} PRIVATE GTest::gtest_main)
  target_compile_options(${name} PRIVATE -Wall -Wextra)
endfunction()

add_unit_test(dvg_test dvg_test.cpp ${ATARI}/dvg.cpp)
add_unit_test(machine_test machine_test.cpp ${BOARD})
add_unit_test(smoke_test smoke_test.cpp ${BOARD} ${WEB}/hwtest_roms.h)

include(GoogleTest)
gtest_discover_tests(dvg_test)
gtest_discover_tests(machine_test)
gtest_discover_tests(smoke_test)
