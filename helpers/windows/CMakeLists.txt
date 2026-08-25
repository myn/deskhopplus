# The Windows helper (#49) — a single portable exe that installs nothing.
#
# Standalone, like tests/: it builds no firmware, needs no SDK and no
# cross-compiler, and the firmware build never reaches it.
#
#   cmake -S helpers/windows -B helpers/windows/build
#   cmake --build helpers/windows/build --config Release
#   ctest --test-dir helpers/windows/build -C Release
#
# Every rule below is ADR-0006, and this file is where it is established rather
# than asserted: a rule that lived only in CI's YAML would be invisible to
# anyone building on a Windows box, which is the drift the ADR exists to stop.

cmake_minimum_required(VERSION 3.20)

# Both languages, deliberately. src/core is C11 and is compiled *as C* — never
# routed through the C++ compiler, which would make this build compile
# something subtly different from what the firmware ships and defeat the reason
# the core is shared at all (#64, ADR-0006).
project(deskhop_helper_windows C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  # Static CRT: no VC++ redistributable to install on the managed laptop.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /utf-8)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core)

# ---------------------------------------------------------------- shared core
#
# Compiled in place. There is no copy of these sources under helpers/ and there
# must never be one — a second copy of the wire format is exactly what #64
# consolidated away, and the same rule Package.swift already follows.
add_library(dhcore STATIC
  ${CORE}/dh_helper.c
  ${CORE}/dh_session.c
  ${CORE}/dh_pair.c
  ${CORE}/dh_frame.c
  ${CORE}/dh_auth.c
  ${CORE}/dh_sha256.c
  ${CORE}/dh_p256.c
  # The end-to-end clipboard seal (#113) and the CLIP_* layouts it seals into.
  # The cipher is not here and is not in the core: ADR-0008 put AES-256-GCM on
  # the platform, which for this helper is src/seal_aead.cpp over CNG.
  ${CORE}/dh_seal.c
  ${CORE}/dh_clip.c
  # The chunked transfer machine (#48) and the CRC32 that is its fidelity
  # check (ADR-0003). Both arrive with the first real payload (#52).
  ${CORE}/dh_xfer.c
  ${CORE}/dh_crc32.c)
target_include_directories(dhcore PUBLIC ${CORE})

if(MSVC)
  # MSVC at /W4 is the strictest of this core's three readings, and that third
  # reading is a large part of what this build is worth (ADR-0006). Warnings it
  # raises here are core defects to fix, so they are errors.
  target_compile_options(dhcore PRIVATE /WX)

  # Except dh_p256.c, which is the one core file that compiles vendored code:
  # it includes micro-ecc's uECC.c itself, so that the curve configuration is
  # set in one place for all three toolchains. Third-party warnings are not
  # ours to fix and silencing them one by one would blunt the flag everywhere
  # else, so this file is read at /W4 and not failed on.
  set_source_files_properties(${CORE}/dh_p256.c PROPERTIES COMPILE_OPTIONS "/wd4127")
  set_property(SOURCE ${CORE}/dh_p256.c APPEND PROPERTY COMPILE_OPTIONS /WX-)
endif()

# ------------------------------------------------------- the ladder, unplatformed
#
# The autostart ladder's *decisions*, with no Win32 in them — which is what
# makes them testable on any machine, and they are the code most likely to be
# wrong on a laptop nobody can reproduce (#86).
add_library(ladder INTERFACE)
target_include_directories(ladder INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

enable_testing()

# House style: an assertion macro, a main, a printed failure line, a non-zero
# exit. No framework and no new dependency (ADR-0006).
add_executable(autostart_ladder_test tests/autostart_ladder_test.cpp)
target_link_libraries(autostart_ladder_test PRIVATE ladder)
add_test(NAME autostart_ladder COMMAND autostart_ladder_test)

if(WIN32)
  # The seal, through the cipher this helper actually ships (#113). Windows
  # only, because BCrypt is — and that is exactly why it is worth having: CNG's
  # AES-GCM agreeing with the golden vectors is a claim no other machine's
  # suite can make for it.
  add_executable(seal_test tests/seal_test.cpp src/seal_aead.cpp)
  target_link_libraries(seal_test PRIVATE dhcore bcrypt)
  target_include_directories(seal_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(seal_test PRIVATE
    DH_TEST_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/../../test-vectors/frames.txt")
  if(MSVC)
    target_compile_options(seal_test PRIVATE /WX)
    # `_Static_assert` for the reason the helper defines it below: the core's
    # headers are C11 and MSVC's C++ compiler does not take the keyword.
    #
    # `_CRT_SECURE_NO_WARNINGS` for `fopen`, which this test uses to read
    # test-vectors/frames.txt. MSVC deprecates it in favour of `fopen_s`, and
    # /WX makes that deprecation an error. Nothing else in this helper reads a
    # file through the C library — the exe uses Win32 — so the exemption is
    # this one test's rather than the build's.
    target_compile_definitions(seal_test PRIVATE _Static_assert=static_assert
                               _CRT_SECURE_NO_WARNINGS)
  endif()
  add_test(NAME seal COMMAND seal_test)

  # The clipboard path, driven as two helpers talking to each other (#52).
  # Windows only for the same reason seal_test is: it needs the cipher this
  # helper actually ships, which is CNG's.
  add_executable(clip_service_test tests/clip_service_test.cpp src/clip_service.cpp
                 src/seal_aead.cpp)
  target_link_libraries(clip_service_test PRIVATE dhcore bcrypt)
  target_include_directories(clip_service_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(MSVC)
    target_compile_options(clip_service_test PRIVATE /WX)
    target_compile_definitions(clip_service_test PRIVATE _Static_assert=static_assert)
  endif()
  add_test(NAME clip_service COMMAND clip_service_test)

  # WIN32 rather than a console app: the helper lives in the notification area
  # and must not flash a console window when it autostarts at logon.
  add_executable(deskhop-helper WIN32
    src/main.cpp
    src/helper_session.cpp
    src/clip_service.cpp
    src/clipboard.cpp
    src/words.cpp
    src/secret_store.cpp
    src/seal_aead.cpp
    src/hid_transport.cpp
    src/autostart.cpp
    src/tray.cpp
    src/app.manifest)

  target_link_libraries(deskhop-helper PRIVATE dhcore ladder
    # Inbox libraries only — anything not already on a stock Windows install is
    # not a candidate (ADR-0006). bcrypt joins the ADR's list for
    # BCryptGenRandom, the documented system RNG; it ships with Windows like
    # the rest and so is governed by the same rule.
    user32 shell32 ole32 setupapi hid cfgmgr32 crypt32 bcrypt)

  if(MSVC)
    target_compile_options(deskhop-helper PRIVATE /WX)

    # The core's headers are C11 and use `_Static_assert`, which MSVC's C++
    # compiler does not accept as a keyword. Spelled as its C++ equivalent
    # rather than by editing the core: the assertion is the firmware's, and a
    # header that reads differently per consumer is the drift #64 removed.
    # Harmless where the compiler does accept the keyword.
    target_compile_definitions(deskhop-helper PRIVATE _Static_assert=static_assert)
  endif()
endif()
