# Host test harness for the pure-logic core (src/core/) — the standalone build
# decided in #9: no SDK, no cross-compiler, no framework, invoked separately so
# the firmware build is untouched. Run in CI by the "Run host tests" step in
# .github/workflows/build.yml, ahead of the firmware build.
#
# The macOS helper's suite (swift run channel-tests) is *not* run there: it
# needs a macOS runner, which is a separate decision.
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

add_executable(relay_test relay_test.c ${CORE}/dh_relay.c ${CORE}/dh_outq.c ${CORE}/dh_frame.c)
target_include_directories(relay_test PRIVATE ${CORE})
target_compile_definitions(relay_test PRIVATE DH_TEST_VECTORS="${VECTORS}")
add_test(NAME relay_test COMMAND relay_test)

# The outbound queue both of the device's outbound seams drain through (#69).
add_executable(outq_test outq_test.c ${CORE}/dh_outq.c ${CORE}/dh_frame.c)
target_include_directories(outq_test PRIVATE ${CORE})
add_test(NAME outq_test COMMAND outq_test)


add_executable(xfer_test xfer_test.c ${CORE}/dh_xfer.c ${CORE}/dh_clip.c
               ${CORE}/dh_crc32.c ${CORE}/dh_frame.c)
target_include_directories(xfer_test PRIVATE ${CORE})
add_test(NAME xfer_test COMMAND xfer_test)

# The configuration's validation decision (#74). Not part of src/core/ — it is
# firmware domain, not the helper↔firmware protocol — but it is pure C with no
# flash access, which is the whole point: the defect it guards shipped because
# the arithmetic was locked behind write_flash_page and could not be reached.
set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_executable(config_test config_test.c ${FW}/config_store.c ${CORE}/dh_crc32.c)
target_include_directories(config_test PRIVATE ${CORE} ${FW}/include)
add_test(NAME config_test COMMAND config_test)

# What the peer board is running, tracked from its heartbeats (#89). Firmware
# domain like config_store.c above, and split out for the same reason: behind
# a UART handler the decision could not be reached by a test.
add_executable(peer_fw_test peer_fw_test.c ${FW}/peer_fw.c)
target_include_directories(peer_fw_test PRIVATE ${FW}/include)
add_test(NAME peer_fw_test COMMAND peer_fw_test)

# Whether an upgrade being received is still alive (#90). Split out for the
# same reason as the two above: the flag that a stalled transfer left set for
# good sat inside a core1 task, where no test could reach the decision.
add_executable(fw_upgrade_test fw_upgrade_test.c ${FW}/fw_upgrade.c)
target_include_directories(fw_upgrade_test PRIVATE ${FW}/include)
add_test(NAME fw_upgrade_test COMMAND fw_upgrade_test)
