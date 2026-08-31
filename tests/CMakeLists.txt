# Host test harness for the pure-logic core (src/core/) — the standalone build
# decided in #9: no SDK, no cross-compiler, no framework, invoked separately so
# the firmware build is untouched. Run in CI by the "Run host tests" step in
# .github/workflows/build.yml, ahead of the firmware build.
#
# The macOS helper's suite (swift run channel-tests) is *not* run there: it
# needs a macOS runner, which is a separate decision.
#
#   cmake -S tests -B tests/build && cmake --build tests/build && ctest --test-dir tests/build

cmake_minimum_required(VERSION 3.20)
project(deskhopplus_core_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
  # The same static CRT the Windows helper links (ADR-0006), carried here in
  # the test harness's own build file rather than as a flag CI passes. A rule
  # that lives only in a YAML file is invisible to anyone building locally on a
  # Windows box, which is the drift the ADR is trying to prevent.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # /W4 is the third reading of this core, and the strictest of the three.
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -pedantic)
endif()

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../src/core)
set(VECTORS ${CMAKE_CURRENT_SOURCE_DIR}/../test-vectors/frames.txt)
set(PRIMITIVE_VECTORS ${CMAKE_CURRENT_SOURCE_DIR}/../test-vectors/primitives.txt)

enable_testing()

add_executable(keyboard_transport_test keyboard_transport_test.c ${CORE}/dh_keyboard_transport.c)
target_include_directories(keyboard_transport_test PRIVATE ${CORE})
add_test(NAME keyboard_transport_test COMMAND keyboard_transport_test)

add_executable(keyboard_output_test keyboard_output_test.c ${CORE}/dh_keyboard_output.c)
target_include_directories(keyboard_output_test PRIVATE ${CORE} ${CMAKE_CURRENT_SOURCE_DIR}/../src/include)
add_test(NAME keyboard_output_test COMMAND keyboard_output_test)

add_executable(mouse_layout_test mouse_layout_test.c ${CORE}/dh_mouse_layout.c)
target_include_directories(mouse_layout_test PRIVATE ${CORE})
add_test(NAME mouse_layout_test COMMAND mouse_layout_test)

add_executable(cursor_trace_test cursor_trace_test.c ${CORE}/dh_cursor_trace.c)
target_include_directories(cursor_trace_test PRIVATE ${CORE})
add_test(NAME cursor_trace_test COMMAND cursor_trace_test)

add_executable(mouse_seam_test mouse_seam_test.c
               cursor_trace_adapter_stub.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/mouse_logic.c ${CORE}/dh_mouse_layout.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/cursor_position.c ${CORE}/dh_seam_map.c)
target_include_directories(mouse_seam_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${CORE}
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/include)
if(NOT MSVC)
  target_link_libraries(mouse_seam_test PRIVATE m)
endif()
add_test(NAME mouse_seam_test COMMAND mouse_seam_test)

add_executable(seam_map_test seam_map_test.c ${CORE}/dh_seam_map.c)
target_include_directories(seam_map_test PRIVATE ${CORE})
add_test(NAME seam_map_test COMMAND seam_map_test)

add_executable(place_test place_test.c ${CORE}/dh_place.c)
target_include_directories(place_test PRIVATE ${CORE})
add_test(NAME place_test COMMAND place_test)

add_executable(cursor_position_test cursor_position_test.c
               cursor_trace_adapter_stub.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/cursor_position.c
               ${CORE}/dh_mouse_layout.c)
target_include_directories(cursor_position_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${CORE}
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src/include)
add_test(NAME cursor_position_test COMMAND cursor_position_test)

add_executable(hotkey_test hotkey_test.c ${CORE}/dh_hotkey.c)
target_include_directories(hotkey_test PRIVATE ${CORE})
add_test(NAME hotkey_test COMMAND hotkey_test)

add_executable(config_text_test config_text_test.c ${CORE}/dh_config_text.c)
target_include_directories(config_text_test PRIVATE ${CORE})
add_test(NAME config_text_test COMMAND config_text_test)

find_program(NODE_EXECUTABLE node REQUIRED)
add_test(NAME webconfig_keymap_test COMMAND ${NODE_EXECUTABLE}
         ${CMAKE_CURRENT_SOURCE_DIR}/webconfig_keymap_test.js
         ${CMAKE_CURRENT_SOURCE_DIR}/../webconfig/config-unpacked.htm)
add_test(NAME webconfig_layout_test COMMAND ${NODE_EXECUTABLE}
         ${CMAKE_CURRENT_SOURCE_DIR}/webconfig_layout_test.js
         ${CMAKE_CURRENT_SOURCE_DIR}/../webconfig/config-unpacked.htm)
add_test(NAME webconfig_cursor_trace_test COMMAND ${NODE_EXECUTABLE}
         ${CMAKE_CURRENT_SOURCE_DIR}/webconfig_cursor_trace_test.js
         ${CMAKE_CURRENT_SOURCE_DIR}/../webconfig/config-unpacked.htm)

add_executable(frame_test frame_test.c ${CORE}/dh_frame.c)
target_include_directories(frame_test PRIVATE ${CORE})
target_compile_definitions(frame_test PRIVATE DH_TEST_VECTORS="${VECTORS}")
add_test(NAME frame_test COMMAND frame_test)

add_executable(txq_test txq_test.c)
target_include_directories(txq_test PRIVATE ${CORE})
add_test(NAME txq_test COMMAND txq_test)

# The session layer speaks v2 (#111), so it is gated by ${VECTORS} again — and
# by ${PRIMITIVE_VECTORS}, which carry the published session key material the
# tags in those frames were computed under. Without the keys the frames could
# only be re-encoded; with them the board's own answers are checked byte for
# byte against what an independent implementation produced.
add_executable(session_test session_test.c ${CORE}/dh_session.c ${CORE}/dh_pair.c
               ${CORE}/dh_frame.c ${CORE}/dh_auth.c ${CORE}/dh_sha256.c ${CORE}/dh_p256.c)
target_include_directories(session_test PRIVATE ${CORE})
target_compile_definitions(session_test PRIVATE DH_TEST_VECTORS="${VECTORS}"
                           DH_PRIMITIVE_VECTORS="${PRIMITIVE_VECTORS}")
add_test(NAME session_test COMMAND session_test)
# The helper's side of the same session (#80). Gated by both vector files for
# the same reason session_test.c is — and it drives the real dh_session as its
# far end, so what it checks is the two ends agreeing rather than this side
# agreeing with a mock of the other.
add_executable(helper_test helper_test.c ${CORE}/dh_helper.c ${CORE}/dh_session.c
               ${CORE}/dh_pair.c ${CORE}/dh_frame.c ${CORE}/dh_auth.c ${CORE}/dh_sha256.c
               ${CORE}/dh_p256.c ${CORE}/dh_outq.c)
target_include_directories(helper_test PRIVATE ${CORE})
target_compile_definitions(helper_test PRIVATE DH_TEST_VECTORS="${VECTORS}"
                           DH_PRIMITIVE_VECTORS="${PRIMITIVE_VECTORS}")
add_test(NAME helper_test COMMAND helper_test)

# The four primitives the board runs, and the frame tag built on them (#110).
# dh_p256.c is the only file that compiles micro-ecc: it sets the curve
# configuration in one place, for all three of this core's toolchains.
add_executable(auth_test auth_test.c ${CORE}/dh_auth.c ${CORE}/dh_sha256.c ${CORE}/dh_p256.c
               ${CORE}/dh_frame.c)
target_include_directories(auth_test PRIVATE ${CORE})
target_compile_definitions(auth_test PRIVATE DH_TEST_VECTORS="${VECTORS}"
                           DH_PRIMITIVE_VECTORS="${PRIMITIVE_VECTORS}")
add_test(NAME auth_test COMMAND auth_test)

# The board's identity and its one registration (#111). Needs the curve: a
# registration is what one ECDH produced, so there is nothing here to test
# without it.
add_executable(pair_test pair_test.c ${CORE}/dh_pair.c ${CORE}/dh_p256.c ${CORE}/dh_sha256.c
               ${CORE}/dh_auth.c ${CORE}/dh_frame.c)
target_include_directories(pair_test PRIVATE ${CORE})
target_compile_definitions(pair_test PRIVATE DH_PRIMITIVE_VECTORS="${PRIMITIVE_VECTORS}")
add_test(NAME pair_test COMMAND pair_test)

add_executable(relay_test relay_test.c ${CORE}/dh_relay.c ${CORE}/dh_outq.c ${CORE}/dh_frame.c)
target_include_directories(relay_test PRIVATE ${CORE})
target_compile_definitions(relay_test PRIVATE DH_TEST_VECTORS="${VECTORS}")
add_test(NAME relay_test COMMAND relay_test)

# The outbound queue both of the device's outbound seams drain through (#69).
add_executable(outq_test outq_test.c ${CORE}/dh_outq.c ${CORE}/dh_frame.c)
target_include_directories(outq_test PRIVATE ${CORE})
add_test(NAME outq_test COMMAND outq_test)

# The other side of the same board: what core 1 reassembles from the peer board
# and parks for core 0 to re-tag (#139).
add_executable(inq_test inq_test.c ${CORE}/dh_inq.c ${CORE}/dh_frame.c)
target_include_directories(inq_test PRIVATE ${CORE})
add_test(NAME inq_test COMMAND inq_test)

# The transfer machine, with every message crossing the real codecs — which
# since #113 means crossing the real seal too. aes_gcm_ref.c is the suite's own
# AES-256-GCM: ADR-0008 put the cipher on the platform, so the C suite carries
# one of its own to open what the core sealed. seal_test.c gates it.
add_executable(xfer_test xfer_test.c aes_gcm_ref.c ${CORE}/dh_xfer.c ${CORE}/dh_clip.c
               ${CORE}/dh_seal.c ${CORE}/dh_crc32.c ${CORE}/dh_frame.c ${CORE}/dh_auth.c
               ${CORE}/dh_sha256.c ${CORE}/dh_p256.c)
target_include_directories(xfer_test PRIVATE ${CORE})
add_test(NAME xfer_test COMMAND xfer_test)

# The end-to-end clipboard seal (#113). Gated by both vector files: the frames
# carry the sealed bodies an independent implementation produced, and the
# primitives carry the key material they were produced from. It drives a real
# dh_session board as the hop in between, so what it checks is a payload
# crossing a board rather than a payload beside one.
add_executable(seal_test seal_test.c aes_gcm_ref.c ${CORE}/dh_seal.c ${CORE}/dh_clip.c
               ${CORE}/dh_crc32.c ${CORE}/dh_frame.c ${CORE}/dh_auth.c ${CORE}/dh_sha256.c
               ${CORE}/dh_p256.c ${CORE}/dh_session.c ${CORE}/dh_pair.c)
target_include_directories(seal_test PRIVATE ${CORE})
target_compile_definitions(seal_test PRIVATE DH_TEST_VECTORS="${VECTORS}"
                           DH_PRIMITIVE_VECTORS="${PRIMITIVE_VECTORS}")
add_test(NAME seal_test COMMAND seal_test)

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

# The board's stored identity (#111). Firmware domain, pure arithmetic, split
# out for the reason #74 paid for — with more at stake, because the key this
# validates cannot be regenerated without unpairing every helper.
add_executable(identity_test identity_test.c ${FW}/identity_store.c ${CORE}/dh_crc32.c)
target_include_directories(identity_test PRIVATE ${CORE} ${FW}/include)
add_test(NAME identity_test COMMAND identity_test)

# Which flash ranges each writer touches, and which of them must never reach
# the identity sector (#111). The failure this gates is the one that would hand
# two boards a single identity, so it is checked as arithmetic a test can run
# rather than left to the linker script alone.
add_executable(flash_layout_test flash_layout_test.c)
target_include_directories(flash_layout_test PRIVATE ${CORE} ${FW}/include)
add_test(NAME flash_layout_test COMMAND flash_layout_test)
