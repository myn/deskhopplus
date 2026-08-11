# Host test harness for the pure-logic core (src/core/) — the standalone build
# decided in #9: no SDK, no cross-compiler, no framework, invoked separately so
# the firmware build is untouched. Runs as its own CI job.
#
#   cmake -S tests -B tests/build && cmake --build tests/build && ctest --test-dir tests/build

cmake_minimum_required(VERSION 3.13)
project(deskhopplus_core_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -pedantic)

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../src/core)
set(VECTORS ${CMAKE_CURRENT_SOURCE_DIR}/../test-vectors/frames.txt)

enable_testing()

add_executable(frame_test frame_test.c ${CORE}/dh_frame.c)
target_include_directories(frame_test PRIVATE ${CORE})
target_compile_definitions(frame_test PRIVATE DH_TEST_VECTORS="${VECTORS}")
add_test(NAME frame_test COMMAND frame_test)

add_executable(txq_test txq_test.c)
target_include_directories(txq_test PRIVATE ${CORE})
add_test(NAME txq_test COMMAND txq_test)

add_executable(session_test session_test.c ${CORE}/dh_session.c ${CORE}/dh_pair.c
               ${CORE}/dh_frame.c)
target_include_directories(session_test PRIVATE ${CORE})
target_compile_definitions(session_test PRIVATE DH_TEST_VECTORS="${VECTORS}")
add_test(NAME session_test COMMAND session_test)
add_executable(pair_test pair_test.c ${CORE}/dh_pair.c)
target_include_directories(pair_test PRIVATE ${CORE})
add_test(NAME pair_test COMMAND pair_test)

add_executable(relay_test relay_test.c ${CORE}/dh_relay.c ${CORE}/dh_frame.c)
target_include_directories(relay_test PRIVATE ${CORE})
target_compile_definitions(relay_test PRIVATE DH_TEST_VECTORS="${VECTORS}")
add_test(NAME relay_test COMMAND relay_test)


add_executable(xfer_test xfer_test.c ${CORE}/dh_xfer.c ${CORE}/dh_clip.c
               ${CORE}/dh_crc32.c ${CORE}/dh_frame.c)
target_include_directories(xfer_test PRIVATE ${CORE})
add_test(NAME xfer_test COMMAND xfer_test)
