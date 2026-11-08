# apkrt: loader and Linux syscall layer for guest code (ARCHITECTURE.md 4.3–4.6).
enable_language(ASM)
# M2 step 4: static programs from container dylibs; step 5: the linker and libdl for dynamic programs
# (ADR-0021), and the Linux personality their test suite needs: files, threads, signals. The
# signal handlers look up the x18 islands (ADR-0020 X4).
set(apkrt_syscall_sources src/syscalls.cpp src/sys_files.cpp src/descriptors.cpp src/sys_memory.cpp src/sys_signals.cpp
  src/sys_events.cpp src/sys_futex.cpp src/sys_threads.cpp src/threads.cpp src/guest_context.cpp src/signals.cpp
  src/islands.cpp src/process_exit.cpp src/host_calls.cpp src/session.cpp src/windows.cpp src/channels.cpp src/audio.cpp
  src/audio_effects.cpp
  src/proc.cpp src/sys_cpu.cpp src/properties.cpp src/sys_sockets.cpp src/sys_memfd.cpp src/hardware_buffers.cpp
  src/netlink.cpp src/cameras.cpp
  src/video_decoders.cpp src/media_players.cpp src/watches.cpp src/sensor_syscalls.cpp src/location_syscalls.cpp
  src/battery.cpp src/network.cpp src/audio_decode.cpp src/audio_codec.cpp)
add_library(apkrt STATIC src/apkrt.cpp src/guest_tls.c src/loader.cpp src/linker.cpp src/dl.cpp src/program.cpp
  src/environment.cpp src/vfs.cpp src/sensor_hub.cpp src/location_hub.cpp src/hwcap.cpp src/dns_proxy.cpp
  src/stall_watchdog.cpp src/monitor_probe.cpp
  ${apkrt_syscall_sources} src/syscall_entry.S)
target_include_directories(apkrt PUBLIC include)
target_compile_definitions(apkrt PRIVATE APKRT_VERSION="${PROJECT_VERSION}")
target_link_libraries(apkrt PRIVATE apk_defaults)
# The window registry retains the host's layers (ADR-0028, decision 6).
target_link_libraries(apkrt PUBLIC "-framework CoreFoundation")
# The syscall layer runs on the guest's stack, which AddressSanitizer does not know as a thread
# stack; instrumented, every exit_group would print a false "stack switching" warning.
# So does libdl's work (dl.cpp and the linker and loader under it), which guest code calls on its
# own stack: when an exception unwinds such a stack, AddressSanitizer reports its own redzones. The
# unit tests of the linker and the loader build instrumented copies (tests/checked_*.cpp).
if(APK_SANITIZE)
  set_source_files_properties(${apkrt_syscall_sources} src/dl.cpp src/linker.cpp src/loader.cpp
    PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
endif()

if(APK_HOST_BUILD)
  # Test support shared with the converter's tests.
  add_library(apk_testing INTERFACE)
  target_include_directories(apk_testing INTERFACE testing)

  add_executable(c_api_test tests/c_api_test.c)
  target_compile_definitions(c_api_test PRIVATE APKRT_EXPECTED_VERSION="${PROJECT_VERSION}")
  target_link_libraries(c_api_test PRIVATE apkrt apk_defaults)
  add_test(NAME runtime.c_api COMMAND c_api_test)

  add_executable(guest_tls_test tests/guest_tls_test.c)
  target_link_libraries(guest_tls_test PRIVATE apkrt apk_defaults)
  add_test(NAME runtime.guest_tls COMMAND guest_tls_test)

  add_executable(stall_watchdog_test tests/stall_watchdog_test.cpp)
  target_include_directories(stall_watchdog_test PRIVATE src)
  target_link_libraries(stall_watchdog_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.stall_watchdog COMMAND stall_watchdog_test)

  # The exclusive-monitor probe (ADR-0020 X7 c2) and the loader's refusal of a library that needs the monitor kept.
  add_executable(monitor_probe_test tests/monitor_probe_test.cpp)
  target_include_directories(monitor_probe_test PRIVATE src)
  target_link_libraries(monitor_probe_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.monitor_probe COMMAND monitor_probe_test)
  set_tests_properties(runtime.monitor_probe PROPERTIES TIMEOUT 120)

  # Loads small containers the converter makes (ADR-0020 X4), so it needs a directory of its own.
  add_executable(loader_test tests/loader_test.cpp tests/checked_loader.cpp)
  target_include_directories(loader_test PRIVATE src "${PROJECT_SOURCE_DIR}/importer/converter/tests")
  target_link_libraries(loader_test PRIVATE apkrt apkconv apk_defaults apk_testing)
  add_test(NAME runtime.loader COMMAND loader_test "${CMAKE_CURRENT_BINARY_DIR}/loader_test_scratch")

  # On the NDK fixtures raw-dyn and libraw-dyn.so, laid out in memory; skipped while they are missing.
  add_executable(linker_test tests/linker_test.cpp tests/checked_linker.cpp tests/checked_loader.cpp)
  target_include_directories(linker_test PRIVATE src)
  target_link_libraries(linker_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.linker COMMAND linker_test "${APK_FIXTURES_DIR}" "${APK_BIONIC_DIR}")
  set_tests_properties(runtime.linker PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(vfs_test tests/vfs_test.cpp)
  target_include_directories(vfs_test PRIVATE src)
  target_link_libraries(vfs_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.vfs COMMAND vfs_test)

  # The sensor hub (ADR-0061 step 1): the table, the per-queue pipes and fan-out, the rate merge, the timestamp
  # shift and the concurrency, driven with a fake host (no CoreMotion, no guest syscalls).
  add_executable(sensor_hub_test tests/sensor_hub_test.cpp)
  target_include_directories(sensor_hub_test PRIVATE src)
  target_link_libraries(sensor_hub_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.sensor_hub COMMAND sensor_hub_test)
  set_tests_properties(runtime.sensor_hub PROPERTIES TIMEOUT 60)

  # The sensor transport (ADR-0061): APKRT_NR_SENSOR through the dispatcher, the queue as a guest descriptor.
  add_executable(sensor_transport_test tests/sensor_transport_test.cpp)
  target_include_directories(sensor_transport_test PRIVATE src)
  target_link_libraries(sensor_transport_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.sensor_transport COMMAND sensor_transport_test)
  set_tests_properties(runtime.sensor_transport PROPERTIES TIMEOUT 60)

  # The permission grant store (ADR-0063, ADR-0064): the host's answers per group, read by the location syscalls.
  add_executable(permissions_test tests/permissions_test.cpp)
  target_include_directories(permissions_test PRIVATE src)
  target_link_libraries(permissions_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.permissions COMMAND permissions_test)

  # The coarse-location fudger (ADR-0063 decision 3): the ~2 km grid snap, the per-app offset and its redraw.
  add_executable(location_fudge_test tests/location_fudge_test.cpp)
  target_include_directories(location_fudge_test PRIVATE src)
  target_link_libraries(location_fudge_test PRIVATE apk_defaults apk_testing)
  add_test(NAME runtime.location_fudge COMMAND location_fudge_test)

  # The location hub (ADR-0063): the continuous slot and sequence, the bounded one-shot table, the host calls and the
  # coarse fudge at delivery, driven with a fake host (no CoreLocation, no session, no syscalls).
  add_executable(location_hub_test tests/location_hub_test.cpp)
  target_include_directories(location_hub_test PRIVATE src)
  target_link_libraries(location_hub_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.location_hub COMMAND location_hub_test)
  set_tests_properties(runtime.location_hub PROPERTIES TIMEOUT 60)

  # The location transport (ADR-0063): APKRT_NR_LOCATION through the dispatcher, the grant gate, and a fix announced
  # through the session event stream and fetched with GET.
  add_executable(location_transport_test tests/location_transport_test.cpp)
  target_include_directories(location_transport_test PRIVATE src)
  target_link_libraries(location_transport_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.location_transport COMMAND location_transport_test)
  set_tests_properties(runtime.location_transport PROPERTIES TIMEOUT 60)

  # The battery transport (App gap-1, 2026-10-29): APKRT_NR_BATTERY (GET) through the dispatcher, and apkrt_battery_set
  # announcing APKRT_EVENT_BATTERY through the session event stream, only on a change and only while a session runs.
  add_executable(battery_transport_test tests/battery_transport_test.cpp)
  target_include_directories(battery_transport_test PRIVATE src)
  target_link_libraries(battery_transport_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.battery_transport COMMAND battery_transport_test)
  set_tests_properties(runtime.battery_transport PROPERTIES TIMEOUT 60)
  # The network transport (ADR-0054 step d)): APKRT_NR_NETWORK (GET) through the dispatcher, and apkrt_network_set
  # announcing APKRT_EVENT_NETWORK in a session.
  add_executable(network_transport_test tests/network_transport_test.cpp)
  target_include_directories(network_transport_test PRIVATE src)
  target_link_libraries(network_transport_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.network_transport COMMAND network_transport_test)
  set_tests_properties(runtime.network_transport PROPERTIES TIMEOUT 60)

  # A host callback (apkrt_network_set and the like) must not abort when it runs during exit's static destruction: the
  # state's mutex is immortal. This hammers it from a detached thread and returns from main.
  add_executable(exit_race_test tests/exit_race_test.cpp)
  target_include_directories(exit_race_test PRIVATE src)
  target_link_libraries(exit_race_test PRIVATE apkrt apk_defaults)
  add_test(NAME runtime.exit_race COMMAND exit_race_test)
  set_tests_properties(runtime.exit_race PROPERTIES TIMEOUT 60)

  # A trim (APKRT_EVENT_TRIM_MEMORY) writes one "apkrt" line with the process's memory footprint to the session log
  # (ADR-0035 item 3). This begins a session, posts a trim and checks the line's shape on stderr.
  add_executable(trim_footprint_test tests/trim_footprint_test.cpp)
  target_link_libraries(trim_footprint_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.trim_footprint COMMAND trim_footprint_test)

  # The SoundPool plan's step 3: APKRT_NR_AUDIO_DECODE through the dispatcher, on soundpool-codec-test's files.
  add_executable(audio_decode_test tests/audio_decode_test.cpp)
  target_include_directories(audio_decode_test PRIVATE src)
  target_compile_definitions(audio_decode_test PRIVATE
    TONE_DIR="${PROJECT_SOURCE_DIR}/fixtures/apps/soundpool-codec-test/assets")
  target_link_libraries(audio_decode_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.audio_decode COMMAND audio_decode_test)
  set_tests_properties(runtime.audio_decode PROPERTIES TIMEOUT 60)

  # ADR-0060 e2f: APKRT_NR_AUDIO_CODEC through the dispatcher, on soundpool-codec-test's tone files,
  # media-player-ogg-test's Ogg Opus files and media-codec-opus-test's surround ones.
  add_executable(audio_codec_test tests/audio_codec_test.cpp)
  target_include_directories(audio_codec_test PRIVATE src)
  target_compile_definitions(audio_codec_test PRIVATE
    TONE_DIR="${PROJECT_SOURCE_DIR}/fixtures/apps/soundpool-codec-test/assets"
    OGG_DIR="${PROJECT_SOURCE_DIR}/fixtures/apps/media-player-ogg-test/assets"
    SURROUND_DIR="${PROJECT_SOURCE_DIR}/fixtures/apps/media-codec-opus-test/assets")
  target_link_libraries(audio_codec_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.audio_codec COMMAND audio_codec_test)
  set_tests_properties(runtime.audio_codec PROPERTIES TIMEOUT 60)

  # The guest's /dev/socket/dnsproxyd (ADR-0054 step b): the replies in DnsProxyListener's bytes.
  add_executable(dns_proxy_test tests/dns_proxy_test.cpp)
  target_include_directories(dns_proxy_test PRIVATE src)
  target_link_libraries(dns_proxy_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.dns_proxy COMMAND dns_proxy_test)

  # The guest's hardware buffers (ADR-0058 a)): the IDs, references and locks against a fake table of host buffers.
  add_executable(hardware_buffers_test tests/hardware_buffers_test.cpp)
  target_include_directories(hardware_buffers_test PRIVATE src)
  target_link_libraries(hardware_buffers_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.hardware_buffers COMMAND hardware_buffers_test)

  # The guest's video decoders (ADR-0060 e2b3): the IDs, limits and hostile values, and a destroy racing a queue,
  # against a fake table of host decoders; the NR numbers read from the headers, each once.
  add_executable(video_decoders_test tests/video_decoders_test.cpp)
  target_include_directories(video_decoders_test PRIVATE src)
  target_compile_definitions(video_decoders_test PRIVATE "APKRT_RUNTIME_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
  target_link_libraries(video_decoders_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.video_decoders COMMAND video_decoders_test)

  # ADR-0073: camera through the dispatcher against a fake table of host cameras.
  add_executable(cameras_test tests/cameras_test.cpp)
  target_include_directories(cameras_test PRIVATE src)
  target_link_libraries(cameras_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.cameras COMMAND cameras_test)

  # ADR-0073 step e: the legacy Camera's parameters (framework/native's camera_parameters.cpp) on the host, against
  # Android 15's emulator's own.
  add_executable(camera_parameters_test tests/camera_parameters_test.cpp
    ../framework/native/android_runtime/camera_parameters.cpp)
  target_include_directories(camera_parameters_test PRIVATE ../framework/native/android_runtime)
  target_link_libraries(camera_parameters_test PRIVATE apk_defaults apk_testing)
  add_test(NAME runtime.camera_parameters COMMAND camera_parameters_test)
  add_executable(media_players_test tests/media_players_test.cpp)
  target_include_directories(media_players_test PRIVATE src)
  target_compile_definitions(media_players_test PRIVATE "APKRT_RUNTIME_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
  target_link_libraries(media_players_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.media_players COMMAND media_players_test)

  # ADR-0060 e2d: libmediandk's ISO BMFF parser on the host, with ASan, against Minato's own test files and hostile
  # ones; the sweep changes and cuts the test files within a time budget, so ctest stays fast.
  add_executable(mp4_extractor_test tests/mp4_extractor_test.cpp android/mp4_extractor.cpp)
  target_include_directories(mp4_extractor_test PRIVATE android)
  target_compile_definitions(mp4_extractor_test PRIVATE "APKRT_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
  target_link_libraries(mp4_extractor_test PRIVATE apk_defaults apk_testing)
  add_test(NAME runtime.mp4_extractor COMMAND mp4_extractor_test)
  add_test(NAME runtime.mp4_extractor_sweep COMMAND mp4_extractor_test --sweep 20)
  set_tests_properties(runtime.mp4_extractor_sweep PROPERTIES TIMEOUT 120)

  # ADR-0060 e2: libmediandk's Ogg Opus reader on the host, with ASan, on media-player-ogg-test's Ogg Opus files and
  # files of pages it builds, and every changed and cut mono file.
  add_executable(ogg_extractor_test tests/ogg_extractor_test.cpp android/ogg_extractor.cpp)
  target_include_directories(ogg_extractor_test PRIVATE android)
  target_compile_definitions(ogg_extractor_test PRIVATE
    OGG_DIR="${PROJECT_SOURCE_DIR}/fixtures/apps/media-player-ogg-test/assets")
  target_link_libraries(ogg_extractor_test PRIVATE apk_defaults apk_testing)
  add_test(NAME runtime.ogg_extractor COMMAND ogg_extractor_test)
  set_tests_properties(runtime.ogg_extractor PROPERTIES TIMEOUT 120)

  # The guest's network interfaces and its NETLINK_ROUTE sockets (ADR-0054 step c): the names, the kernel's bytes.
  add_executable(netlink_test tests/netlink_test.cpp)
  target_include_directories(netlink_test PRIVATE src)
  target_link_libraries(netlink_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.netlink COMMAND netlink_test)

  add_executable(exit_test tests/exit_test.cpp)
  target_include_directories(exit_test PRIVATE src)
  target_link_libraries(exit_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.exit COMMAND exit_test)

  add_executable(syscalls_test tests/syscalls_test.cpp)
  target_include_directories(syscalls_test PRIVATE src)
  target_link_libraries(syscalls_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.syscalls COMMAND syscalls_test)

  # The guest's descriptor table (ADR-0027).
  add_executable(descriptors_test tests/descriptors_test.cpp)
  target_include_directories(descriptors_test PRIVATE src)
  target_link_libraries(descriptors_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.descriptors COMMAND descriptors_test)
  # A read that waits for good, instead of answering EAGAIN, fails the test within a minute.
  set_tests_properties(runtime.descriptors PROPERTIES TIMEOUT 60)

  # eventfd, epoll, ppoll and pselect6 (ADR-0027).
  add_executable(events_test tests/events_test.cpp)
  target_include_directories(events_test PRIVATE src)
  target_link_libraries(events_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.events COMMAND events_test)
  set_tests_properties(runtime.events PROPERTIES TIMEOUT 60)

  add_executable(apk-run tools/apk_run.cpp)
  target_link_libraries(apk-run PRIVATE apkrt apk_defaults)

  # apk-run's frame clock (tools/frame_clock.h) on the test's own clock.
  add_executable(frame_clock_test tests/frame_clock_test.cpp)
  target_include_directories(frame_clock_test PRIVATE tools)
  target_link_libraries(frame_clock_test PRIVATE apk_defaults apk_testing)
  add_test(NAME runtime.frame_clock COMMAND frame_clock_test)

  # apk-run's host of the system pickers (tools/pick_host.h, ADR-0057): nothing is written through the app's links.
  add_executable(pick_host_test tests/pick_host_test.cpp)
  target_include_directories(pick_host_test PRIVATE tools)
  target_link_libraries(pick_host_test PRIVATE apk_defaults apk_testing)
  add_test(NAME runtime.pick_host COMMAND pick_host_test)

  # apk-run's headless host (tools/headless_host.h): the session's surfaces and the windows' buffers, recorded.
  add_executable(headless_host_test tests/headless_host_test.cpp)
  target_include_directories(headless_host_test PRIVATE tools)
  target_link_libraries(headless_host_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.headless_host COMMAND headless_host_test)

  add_executable(threads_test tests/threads_test.cpp tests/threads_probe.S)
  target_include_directories(threads_test PRIVATE src)
  target_link_libraries(threads_test PRIVATE apkrt apk_defaults apk_testing)
  # The children run the test's functions on stacks the test maps itself, like guest threads.
  if(APK_SANITIZE)
    set_source_files_properties(tests/threads_test.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
  endif()
  add_test(NAME runtime.threads COMMAND threads_test)
  set_tests_properties(runtime.threads PROPERTIES TIMEOUT 60)

  # Signals against the switches between guest and runtime code (ADR-0025), on clone children.
  add_executable(transitions_test tests/transitions_test.cpp tests/transitions_probe.S tests/threads_probe.S
    tests/syscall_entry_probe.S)
  target_include_directories(transitions_test PRIVATE src)
  target_link_libraries(transitions_test PRIVATE apkrt apk_defaults apk_testing)
  if(APK_SANITIZE)
    set_source_files_properties(tests/transitions_test.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
  endif()
  add_test(NAME runtime.transitions COMMAND transitions_test)
  set_tests_properties(runtime.transitions PROPERTIES TIMEOUT 60)

  # Calls from guest code into host code (ADR-0030, test 3): the entry, binding host tables and
  # callbacks, on clone children.
  add_executable(host_call_test tests/host_call_test.cpp tests/host_call_probe.S tests/transitions_probe.S
    tests/threads_probe.S)
  target_include_directories(host_call_test PRIVATE src)
  target_link_libraries(host_call_test PRIVATE apkrt apk_defaults apk_testing)
  if(APK_SANITIZE)
    set_source_files_properties(tests/host_call_test.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
  endif()
  add_test(NAME runtime.host_call COMMAND host_call_test)
  set_tests_properties(runtime.host_call PROPERTIES TIMEOUT 60)

  # The program's start on a thread of its own and its end with the session's handler and the stopped
  # threads (ADR-0028, test 1): each case in a child process, on a static program the converter makes
  # and on clone children.
  add_executable(program_test tests/program_test.cpp tests/threads_probe.S tests/blocking_probe.S
    tests/host_call_probe.S)
  target_include_directories(program_test PRIVATE src "${PROJECT_SOURCE_DIR}/importer/converter/tests")
  target_link_libraries(program_test PRIVATE apkrt apkconv apk_defaults apk_testing)
  if(APK_SANITIZE)
    set_source_files_properties(tests/program_test.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
  endif()
  add_test(NAME runtime.program COMMAND program_test "${CMAKE_CURRENT_BINARY_DIR}/program_test_scratch"
    "${APK_FIXTURES_DIR}")
  set_tests_properties(runtime.program PROPERTIES TIMEOUT 120)

  # The NDK libraries Minato writes itself (ADR-0028): every generated stub has its value, and the built
  # libandroid.so, libnativewindow.so and libz.so export exactly the names of their NDK stubs.
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME runtime.android_libraries
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/android/stubgen.py" check --built "${APK_BIONIC_DIR}")
  set_tests_properties(runtime.android_libraries PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:")

  # The NDK libraries built from bionic's own modules (the compatibility plan's gap 14): libstdc++.so exports exactly
  # the names of the NDK's stub, with their versions, and the guest's public.libraries.txt lists it.
  add_test(NAME runtime.ndk_libraries
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/ndk_libraries_test.py" "${APK_BIONIC_DIR}"
      "${APK_GUEST_SYSTEM_DIR}")
  set_tests_properties(runtime.ndk_libraries PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:")

  # tools/fetch-sources.py's two lists (ADR-0021, ADR-0033 a): the guest libc's by default, ART's with --set art,
  # against a host in memory.
  add_test(NAME runtime.fetch_sources COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/fetch_sources_test.py")

  # ART as guest code from tools/build-art.sh (ADR-0033 a): its libraries link within the set, apk-convert takes them,
  # and the boot jars hold the classes ART checks at start. Skipped while the build is missing.
  set(APK_ART_DIR "${PROJECT_SOURCE_DIR}/build/art" CACHE PATH "Directory of the built ART (tools/build-art.sh)")
  add_test(NAME runtime.art_build
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_build_test.py" "${APK_ART_DIR}"
      "${APK_BIONIC_DIR}" $<TARGET_FILE:apk-convert> "${PROJECT_SOURCE_DIR}/build/sources")
  set_tests_properties(runtime.art_build PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:" TIMEOUT 600)

  # The stamp of ART's sources (tools/art_sources.py), which the checks on build/art compare with the tree.
  add_test(NAME runtime.art_sources COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_sources_test.py")

  # The Mac harness's finalizer timeout for the run-tests (60 s, a test's own value kept); skipped without AOSP's art/test.
  add_test(NAME runtime.art_run_tests_options
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run_tests_options_test.py" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(runtime.art_run_tests_options PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")

  # BoringSSL as libjavacrypto links it (ADR-0046 b): boringssl-test from ART's build prints known answers, whether
  # AES-GCM runs on the crypto extension, and libssl's default cipher order, as on Android 15
  # (fixtures/expected/boringssl-test.out). Skipped while the program is not built.
  set(boringssl_system_libraries libc libm libdl ld-android liblog libnetd_client)
  list(TRANSFORM boringssl_system_libraries REPLACE "(.+)" "${APK_BIONIC_DIR}/\\1.so")
  add_test(NAME runtime.boringssl
    COMMAND ${CMAKE_COMMAND} -D CONVERT=$<TARGET_FILE:apk-convert> -D RUN=$<TARGET_FILE:apk-run>
      -D FIXTURE=${APK_ART_DIR}/bin/boringssl-test "-D SYSTEM_LIBRARIES=${boringssl_system_libraries}"
      -D WORK=${CMAKE_CURRENT_BINARY_DIR}/boringssl_runs -D SYSTEM=${APK_GUEST_SYSTEM_DIR}
      -D EXPECTED_OUTPUT=${PROJECT_SOURCE_DIR}/fixtures/expected/boringssl-test.out
      -P ${PROJECT_SOURCE_DIR}/tools/run-fixture.cmake)
  set_tests_properties(runtime.boringssl PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")

  # The guest builds plan their trees when CMake configures them (tools/bionic-plan.py, tools/art-plan.py,
  # tools/hwui-plan.py): a change to the planner or a module it imports, or a file that comes into the overlay folder
  # or leaves it, configures them again. Skipped while no build is configured and up to date.
  set(APK_HWUI_DIR "${PROJECT_SOURCE_DIR}/build/hwui" CACHE PATH "Directory of the built hwui (tools/build-hwui.sh)")
  add_test(NAME runtime.plan_inputs
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/plan_inputs_test.py" "${APK_BIONIC_DIR}/.."
      "${APK_ART_DIR}" "${APK_HWUI_DIR}")
  set_tests_properties(runtime.plan_inputs PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")

  # The guest's export table (ADR-0055 decision 3, tools/export-table.py), which the import's compatibility check
  # reads: for bionic's libraries and those of ART, the front libs, hwui and the framework that are built, each block
  # is what llvm-nm and llvm-readelf read from its library, and the stub marks are stubgen's.
  add_test(NAME runtime.export_table
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/export_table_test.py"
      "${PROJECT_SOURCE_DIR}/tools/export-table.py" "${APK_BIONIC_DIR}" "${APK_ART_DIR}/lib"
      "${PROJECT_SOURCE_DIR}/build/graphics/guest/bin" "${APK_HWUI_DIR}/lib"
      "${PROJECT_SOURCE_DIR}/build/framework-native")
  set_tests_properties(runtime.export_table PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")
  # tools/guest-check.py's link check (ADR-0052) and its export table check, on Guest/ trees the test makes.
  add_test(NAME runtime.guest_check
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/tests/guest_check_test.py")

  # tools/art-plan.py writes boot_jars.json only when its content changes, so that planning again at a configuration
  # does not build the boot jars again. Skipped without ART's sources.
  add_test(NAME runtime.boot_jars_plan
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/boot_jars_plan_test.py"
      "${PROJECT_SOURCE_DIR}/tools/art-plan.py" "${PROJECT_SOURCE_DIR}/build/sources"
      "${CMAKE_CURRENT_BINARY_DIR}/boot_jars_plan")
  set_tests_properties(runtime.boot_jars_plan PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:")

  # Where ART's heap window places its mappings (ADR-0033 decision 3), with ART's own header for it.
  add_executable(heap_window_test tests/heap_window_test.cpp)
  target_include_directories(heap_window_test PRIVATE art/overlay/art/libartbase/base)
  target_link_libraries(heap_window_test PRIVATE apk_defaults apk_testing)
  add_test(NAME runtime.heap_window COMMAND heap_window_test)

  # The heap window's implicit null-check bound (ADR-0053 decision 3), with ART's own header for it.
  add_executable(heap_window_null_test tests/heap_window_null_test.cpp)
  target_include_directories(heap_window_null_test PRIVATE art/overlay/art/libartbase/base)
  target_link_libraries(heap_window_null_test PRIVATE apk_defaults apk_testing)
  add_test(NAME runtime.heap_window_null COMMAND heap_window_null_test)

  # The property area's device and version values as android.os.Build reads them (ADR-0039 decision 9).
  add_executable(properties_test tests/properties_test.cpp)
  target_include_directories(properties_test PRIVATE src)
  target_link_libraries(properties_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.properties COMMAND properties_test)

  # The CPU features the guest is told (ADR-0047 a): the table from the device's sysctl to AT_HWCAP and AT_HWCAP2.
  add_executable(hwcap_test tests/hwcap_test.cpp)
  target_include_directories(hwcap_test PRIVATE src)
  target_link_libraries(hwcap_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.hwcap COMMAND hwcap_test)

  # Java programs on ART as guest code (ADR-0033 b), in ART's release and debug flavors: each prints what
  # dalvikvm64 -Xint prints on Android 15 (fixtures/expected/<program>.out). Test 1 is art-hello; test 2 is
  # codec-test, with its JNI library and the offsets it saw, which must lie in the top of the heap window, and in the
  # release flavor with libnativeloader's lines that name a namespace, which must be Android 15's (ADR-0040 test 4);
  # the debug flavor loads libopenjdkd.so. The jars come from tools/build-art.sh (<art>/test); skipped while they are
  # missing.
  foreach(flavor IN ITEMS release debug)
    set(suffix "")
    set(flag "")
    set(namespaces --nativeloader "${PROJECT_SOURCE_DIR}/fixtures/expected/codec-test.nativeloader")
    if(flavor STREQUAL "debug")
      set(suffix "_debug")
      set(flag "--debug")
      set(namespaces "")
    endif()
    add_test(NAME runtime.art_hello${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
        "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
        $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
        "${APK_ART_DIR}/test/art-hello.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/art-hello.out" ${flag})
    # Only a first line skips: a failure shows ART's log, where libnativeloader writes "skipped:" too.
    set_tests_properties(runtime.art_hello${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 400)
    add_test(NAME runtime.art_codec${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
        "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
        $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
        "${APK_ART_DIR}/test/codec-test.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/codec-test.out" ${flag}
        --library "${APK_FIXTURES_DIR}/libcodec-test.so" --offsets ${namespaces})
    set_tests_properties(runtime.art_codec${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 400)
    # The same under CMS, the collector app_process gives an app (ADR-0033 decision 5): --gc checks ART's start line.
    add_test(NAME runtime.art_codec_cms${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
        "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
        $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
        "${APK_ART_DIR}/test/codec-test.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/codec-test.out" ${flag}
        --library "${APK_FIXTURES_DIR}/libcodec-test.so" --offsets --gc CMS)
    set_tests_properties(runtime.art_codec_cms${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 400)
    # The same in only the window's upper 2 GiB, as without increased-memory-limit on iOS (a SideStore install with
    # a free account, ADR-0033 d1): patch 0002's test switch makes the Mac reserve just those.
    add_test(NAME runtime.art_codec_upper_half${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
        "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
        $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
        "${APK_ART_DIR}/test/codec-test.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/codec-test.out" ${flag}
        --library "${APK_FIXTURES_DIR}/libcodec-test.so" --offsets --upper-half)
    set_tests_properties(runtime.art_codec_upper_half${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:"
      TIMEOUT 400)
    # Conscrypt (ADR-0046 c): conscrypt-test uses Conscrypt's provider, from conscrypt.jar and libjavacrypto.so in
    # Conscrypt's APEX folders, as on Android 15.
    add_test(NAME runtime.art_conscrypt${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
        "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
        $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
        "${APK_ART_DIR}/test/conscrypt-test.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/conscrypt-test.out"
        ${flag} --conscrypt)
    set_tests_properties(runtime.art_conscrypt${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 400)
    # The JCA and an https URL (ADR-0046 d): crypto-test needs every provider of security.properties and okhttp's
    # URL handlers, so okhttp's and BouncyCastle's jars beside ART's and Conscrypt's APEX, as on Android 15.
    add_test(NAME runtime.art_crypto${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
        "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
        $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
        "${APK_ART_DIR}/test/crypto-test.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/crypto-test.out" ${flag}
        --conscrypt --okhttp --bouncycastle)
    set_tests_properties(runtime.art_crypto${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 400)
    # Networking through Java on loopback (ADR-0054 step c2): net-test's sockets, channels, okhttp's HttpURLConnection
    # and TLS, so with the same jars as crypto-test.
    add_test(NAME runtime.art_net${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
        "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
        $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
        "${APK_ART_DIR}/test/net-test.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/net-test.out" ${flag}
        --conscrypt --okhttp --bouncycastle)
    set_tests_properties(runtime.art_net${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 400)
    # Stress (ADR-0033 test 4): stress-test runs three parts at once, with its JNI library: threads, monitors and
    # allocation; SIGUSR2 handlers while ART stops the world; Boehm's stop-the-world with SIGPWR and SIGXCPU in native
    # code. --stress checks that a signal was handled while the main thread collected.
    add_test(NAME runtime.art_stress${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
        "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
        $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
        "${APK_ART_DIR}/test/stress-test.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/stress-test.out" ${flag}
        --library "${APK_FIXTURES_DIR}/libstress-test.so" --stress)
    set_tests_properties(runtime.art_stress${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 400)
    # The same under CMS, the collector app_process gives an app (ADR-0033 decision 5).
    add_test(NAME runtime.art_stress_cms${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
        "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
        $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
        "${APK_ART_DIR}/test/stress-test.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/stress-test.out" ${flag}
        --library "${APK_FIXTURES_DIR}/libstress-test.so" --stress --gc CMS)
    set_tests_properties(runtime.art_stress_cms${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 400)
    # The same programs under nterp, ART's default interpreter, instead of -Xint (M6 a, ADR-0053 step 3, patch 0008):
    # art-hello and nterp-test (interface calls, null receivers, monitors) in the default window and in the upper
    # half, codec-test in the default window, in the upper half and under CMS, and stress-test under CMS. Their output
    # is the -Xint programs'. The run-tests under nterp need MINATO_ART_NTERP=1 (runtime/tests/art_run_tests.py).
    foreach(nterp_program IN ITEMS art-hello nterp-test)
      string(REPLACE "-" "_" nterp_name "${nterp_program}")
      add_test(NAME runtime.${nterp_name}_nterp${suffix}
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
          "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
          $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
          "${APK_ART_DIR}/test/${nterp_program}.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/${nterp_program}.out"
          ${flag} --nterp)
      set_tests_properties(runtime.${nterp_name}_nterp${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:"
        TIMEOUT 400)
    endforeach()
    add_test(NAME runtime.art_hello_nterp_upper_half${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
        "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
        $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
        "${APK_ART_DIR}/test/art-hello.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/art-hello.out" ${flag}
        --nterp --upper-half)
    set_tests_properties(runtime.art_hello_nterp_upper_half${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:"
      TIMEOUT 400)
    # The same programs compiled (M6 c2, ADR-0053 patch 0009): our dex2oat makes the oat of each with
    # --compiler-filter=speed, ART loads it as a container and runs the compiled methods, with what the interpreter
    # prints: art-hello and nterp-test in the default window, art-hello in the upper half, nterp-test under CMS, and
    # codegen-test (instanceof, check-cast, interfaces, arrays, fields, exceptions, allocation, the card marks) in
    # both windows and under CMS.
    foreach(compiled_run IN ITEMS "art-hello" "nterp-test" "art-hello|--upper-half" "nterp-test|--gc|CMS"
            "codegen-test" "codegen-test|--upper-half" "codegen-test|--gc|CMS")
      string(REPLACE "|" ";" compiled_args "${compiled_run}")
      list(POP_FRONT compiled_args compiled_program)
      string(REPLACE "-" "_" compiled_name "${compiled_program}")
      if("--upper-half" IN_LIST compiled_args)
        set(compiled_name "${compiled_name}_upper_half")
      elseif(compiled_args)
        set(compiled_name "${compiled_name}_cms")
      endif()
      add_test(NAME runtime.${compiled_name}_compiled${suffix}
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
          "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
          $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
          "${APK_ART_DIR}/test/${compiled_program}.jar" Main
          "${PROJECT_SOURCE_DIR}/fixtures/expected/${compiled_program}.out" ${flag} --compiled ${compiled_args})
      set_tests_properties(runtime.${compiled_name}_compiled${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:"
        TIMEOUT 600)
    endforeach()
    foreach(nterp_mode IN ITEMS "" "_upper_half" "_cms")
      set(nterp_args --offsets)
      if(nterp_mode STREQUAL "_upper_half")
        set(nterp_args --offsets --upper-half)
      elseif(nterp_mode STREQUAL "_cms")
        set(nterp_args --offsets --gc CMS)
      endif()
      add_test(NAME runtime.art_codec_nterp${nterp_mode}${suffix}
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
          "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
          $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
          "${APK_ART_DIR}/test/codec-test.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/codec-test.out" ${flag}
          --nterp --library "${APK_FIXTURES_DIR}/libcodec-test.so" ${nterp_args})
      set_tests_properties(runtime.art_codec_nterp${nterp_mode}${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:"
        TIMEOUT 400)
    endforeach()
    add_test(NAME runtime.art_stress_nterp_cms${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}" "${APK_BIONIC_DIR}"
        "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat"
        $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
        "${APK_ART_DIR}/test/stress-test.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/stress-test.out" ${flag}
        --nterp --library "${APK_FIXTURES_DIR}/libstress-test.so" --stress --gc CMS)
    set_tests_properties(runtime.art_stress_nterp_cms${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:"
      TIMEOUT 400)
    # ART's run-tests (ADR-0033 test 3): the sample of fixtures/art-run-tests.txt, one after another, each printing
    # what Android 15 prints; the whole subset is a check of its own (runtime/tests/art_run_tests.py without
    # --sample). The first run builds the sample's jars, about 0.5 s each. These runs and the compiled ones, in both
    # flavors, share art_runs' jars and run-tests folders, so they take one lock and run one at a time.
    add_test(NAME runtime.art_run_tests${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run_tests.py" "${APK_ART_DIR}"
        "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}"
        "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat" $<TARGET_FILE:apk-convert>
        $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs" "${PROJECT_SOURCE_DIR}" --sample ${flag})
    set_tests_properties(runtime.art_run_tests${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 600
      RESOURCE_LOCK art_run_tests)
    # The run-tests of the sample compiled (M6 c2, patches 0009 and 0010): the sample's tests but 107-int-math2, whose
    # stack overflow is the signals step's, each with the oat our dex2oat makes at --compiler-filter=speed.
    file(STRINGS "${PROJECT_SOURCE_DIR}/fixtures/art-run-tests.txt" run_test_lines REGEX " sample$")
    set(compiled_sample "")
    foreach(run_test_line IN LISTS run_test_lines)
      string(REGEX REPLACE " .*" "" run_test_name "${run_test_line}")
      if(NOT run_test_name STREQUAL "107-int-math2")
        list(APPEND compiled_sample "${run_test_name}")
      endif()
    endforeach()
    add_test(NAME runtime.art_run_tests_compiled${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run_tests.py" "${APK_ART_DIR}"
        "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}"
        "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat" $<TARGET_FILE:apk-convert>
        $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs" "${PROJECT_SOURCE_DIR}"
        --compiled --jobs 2 ${flag} ${compiled_sample})
    set_tests_properties(runtime.art_run_tests_compiled${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:"
      TIMEOUT 900 RESOURCE_LOCK art_run_tests)
    # codec-test and stress-test compiled: JNI references across collections, System.arraycopy, Unsafe, signals inside
    # a collection. The debug flavor's OatFileManager checks that an oat is mapped once, and codec-test's second class
    # loader loads the same oat again (FORCE_LOAD gives the same image, ADR-0053 step b), so codec-test compiled is
    # the release flavor's.
    if("${flag}" STREQUAL "")
      add_test(NAME runtime.art_codec_compiled${suffix}
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}"
          "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}"
          "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat" $<TARGET_FILE:apk-convert>
          $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
          "${APK_ART_DIR}/test/codec-test.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/codec-test.out"
          --compiled --library "${APK_FIXTURES_DIR}/libcodec-test.so" --offsets)
      set_tests_properties(runtime.art_codec_compiled${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:"
        TIMEOUT 600)
    endif()
    add_test(NAME runtime.art_stress_compiled_cms${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/art_run.py" "${APK_ART_DIR}"
        "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}"
        "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat" $<TARGET_FILE:apk-convert>
        $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs"
        "${APK_ART_DIR}/test/stress-test.jar" Main "${PROJECT_SOURCE_DIR}/fixtures/expected/stress-test.out" ${flag}
        --compiled --library "${APK_FIXTURES_DIR}/libstress-test.so" --stress --gc CMS)
    set_tests_properties(runtime.art_stress_compiled_cms${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:"
      TIMEOUT 600)
    # dex2oat as a guest program (M6 c1): it starts and prints its usage, which names --compiler-filter. It fails
    # while ART is built without bin/dex2oat (dex2oatd in the debug flavor).
    add_test(NAME runtime.dex2oat_starts${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/dex2oat_starts.py" "${APK_ART_DIR}"
        "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}"
        "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat" $<TARGET_FILE:apk-convert>
        $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs" ${flag})
    set_tests_properties(runtime.dex2oat_starts${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 400)
    # dex2oat writes a verify oat of art-hello.jar, imageless with our boot jars (M6 c1, test 2): the odex is an oat
    # that ART accepts for them (header, filter, boot class path checksums) and importer/converter's oat_test converts
    # and loads it.
    add_test(NAME runtime.dex2oat_verify${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/dex2oat_verify.py" "${APK_ART_DIR}"
        "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}"
        "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat" $<TARGET_FILE:apk-convert>
        $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs" "${APK_ART_DIR}/test/art-hello.jar"
        $<TARGET_FILE:oat_test> ${flag})
    set_tests_properties(runtime.dex2oat_verify${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 600)
    # Two dex2oat calls side by side, in one work folder, as CTest starts the compiled tests and dex2oat_verify, make
    # the odex of their own jar (runtime/tests/dex2oat_parallel.py).
    add_test(NAME runtime.dex2oat_parallel${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/dex2oat_parallel.py" "${APK_ART_DIR}"
        "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}"
        "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat" $<TARGET_FILE:apk-convert>
        $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs" "${APK_ART_DIR}/test/art-hello.jar"
        "${APK_ART_DIR}/test/nterp-test.jar" ${flag})
    set_tests_properties(runtime.dex2oat_parallel${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 600)
    # ART's tests in the app (ADR-0033 d): the stage that tools/embed-fixtures.sh puts into fixture builds, with the
    # Java fixtures and the run-tests' sample, run on the Mac as the app's ART mode runs it
    # (runtime/tests/device_art.py).
    add_test(NAME runtime.device_art${suffix}
      COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/device_art.py" selftest "${APK_ART_DIR}"
        "${APK_FIXTURES_DIR}" "${PROJECT_SOURCE_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}"
        "${PROJECT_SOURCE_DIR}/build/sources/icu/icu4c/source/stubdata/icudt75l.dat" $<TARGET_FILE:apk-convert>
        $<TARGET_FILE:apk-run> "${CMAKE_CURRENT_BINARY_DIR}/art_runs" ${flag})
    set_tests_properties(runtime.device_art${suffix} PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 900)
  endforeach()

  # A program in /system/bin and ART's folder (ADR-0039 a, ADR-0040 decision 2): the fixture ns-test reaches a
  # library there only through [system]'s link list, and that library finds its own in its folder. In a second run
  # it opens a library in Conscrypt's folder through the exported namespace com_android_conscrypt (ADR-0046 a).
  add_test(NAME runtime.system_links
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/ns_run.py" "${APK_FIXTURES_DIR}"
      "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run>
      "${CMAKE_CURRENT_BINARY_DIR}/ns_runs" "${PROJECT_SOURCE_DIR}/fixtures/expected/ns-test.out")
  set_tests_properties(runtime.system_links PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:" TIMEOUT 120)

  # apk-run's --app-environment (ADR-0039 decision 2): the fixture env-test starts with init's environment
  # alone, as an app on Android 15, without apk-run's shell defaults.
  add_test(NAME runtime.app_environment
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/environment_run.py" "${APK_FIXTURES_DIR}"
      "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run>
      "${CMAKE_CURRENT_BINARY_DIR}/environment_runs")
  set_tests_properties(runtime.app_environment PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:" TIMEOUT 120)

  # The CPU features a program learns (ADR-0047 a and c): hwcap-test's getauxval, ifunc resolver and /proc/cpuinfo
  # against the bits this Mac's sysctl gives, mapped by the check's own copy of the table.
  add_test(NAME runtime.hwcap_fixture
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/hwcap_check.py" "${APK_FIXTURES_DIR}"
      "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run>
      "${CMAKE_CURRENT_BINARY_DIR}/hwcap_runs")
  set_tests_properties(runtime.hwcap_fixture PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:" TIMEOUT 120)

  # ADR-0047 b1's probe: el0-check's lines against what this Mac's sysctl lets one expect of decision 3's features,
  # which the runtime does not report yet.
  add_test(NAME runtime.el0_check_fixture
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/el0_check.py" "${APK_FIXTURES_DIR}"
      "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run>
      "${CMAKE_CURRENT_BINARY_DIR}/el0_runs")
  set_tests_properties(runtime.el0_check_fixture PROPERTIES SKIP_REGULAR_EXPRESSION "skipped:" TIMEOUT 120)

  # apk-run's --configuration (ADR-0039, config-test): the fixture session-test sees the session it begins through
  # session_configuration and libandroid, and no session without it.
  add_test(NAME runtime.session_run
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/session_run.py" "${APK_FIXTURES_DIR}"
      "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run>
      "${CMAKE_CURRENT_BINARY_DIR}/session_runs")
  set_tests_properties(runtime.session_run PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)

  # The loader's resolve log (ADR-0066 step 0): the imports and dlsyms of the names MINATO_RESOLVE_LOG watches.
  add_test(NAME runtime.resolve_log
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/resolve_log_run.py" "${APK_FIXTURES_DIR}"
      "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run>
      "${CMAKE_CURRENT_BINARY_DIR}/resolve_log_runs")
  set_tests_properties(runtime.resolve_log PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)

  # The FPS unlock (ADR-0066, steps 1 and 2): the targetFrameRate setter raised by import and by dlsym.
  add_test(NAME runtime.frame_rate
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_rate_run.py" "${APK_FIXTURES_DIR}"
      "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run>
      "${CMAKE_CURRENT_BINARY_DIR}/frame_rate_runs" "${PROJECT_SOURCE_DIR}/fixtures/expected/frame-cap-test.out")
  set_tests_properties(runtime.frame_rate PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)

  # The LuaJIT switch (guest JIT engines switched to their interpreter): luaL_openlibs of a LuaJIT, then luaJIT_setmode off.
  add_test(NAME runtime.luajit_switch
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/luajit_switch_run.py" "${APK_FIXTURES_DIR}"
      "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run>
      "${CMAKE_CURRENT_BINARY_DIR}/luajit_switch_runs" "${PROJECT_SOURCE_DIR}/fixtures/expected/luajit-switch-test.out")
  set_tests_properties(runtime.luajit_switch PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)
  add_test(NAME runtime.ffi_closures
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_closures_run.py" "${APK_FIXTURES_DIR}"
      "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run>
      "${CMAKE_CURRENT_BINARY_DIR}/ffi_closures_runs" "${PROJECT_SOURCE_DIR}/fixtures/expected/ffi-closure-test.out")
  set_tests_properties(runtime.ffi_closures PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)

  # The JavaScriptCore switch: JSC_useJIT=0 and JavaScriptCoreUseJIT=0 set before the library's constructors run.
  add_test(NAME runtime.jsc_switch
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/jsc_switch_run.py" "${APK_FIXTURES_DIR}"
      "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run>
      "${CMAKE_CURRENT_BINARY_DIR}/jsc_switch_runs" "${PROJECT_SOURCE_DIR}/fixtures/expected/jsc-switch-test.out")
  set_tests_properties(runtime.jsc_switch PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)

  # getaddrinfo without a network (the corpus profile's "no network"): the failed lookup answers as Android does.
  add_test(NAME runtime.gai_offline
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/gai_offline_run.py" "${APK_FIXTURES_DIR}"
      "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" $<TARGET_FILE:apk-convert> $<TARGET_FILE:apk-run>
      "${CMAKE_CURRENT_BINARY_DIR}/gai_offline_runs" "${PROJECT_SOURCE_DIR}/fixtures/expected/gai-offline-test.out")
  set_tests_properties(runtime.gai_offline PROPERTIES SKIP_REGULAR_EXPRESSION "^skipped:" TIMEOUT 120)

  # Blocking syscalls against signals (ADR-0026), on host threads and on clone children.
  add_executable(blocking_test tests/blocking_test.cpp tests/blocking_probe.S tests/threads_probe.S)
  target_include_directories(blocking_test PRIVATE src)
  target_link_libraries(blocking_test PRIVATE apkrt apk_defaults apk_testing)
  if(APK_SANITIZE)
    set_source_files_properties(tests/blocking_test.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
  endif()
  add_test(NAME runtime.blocking COMMAND blocking_test)
  set_tests_properties(runtime.blocking PROPERTIES TIMEOUT 120)

  # The session's channel and the window registry with its gate (ADR-0028, test 1), on host threads and,
  # for the signals at the gate, on clone children.
  add_executable(session_test tests/session_test.cpp)
  target_include_directories(session_test PRIVATE src)
  target_link_libraries(session_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.session COMMAND session_test)
  set_tests_properties(runtime.session PROPERTIES TIMEOUT 60)

  # The vsync and input channels (ADR-0036).
  add_executable(channels_test tests/channels_test.cpp)
  target_include_directories(channels_test PRIVATE src)
  target_link_libraries(channels_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.channels COMMAND channels_test)
  set_tests_properties(runtime.channels PROPERTIES TIMEOUT 60)

  # The WebView channel (ADR-0056 step a).
  add_executable(webview_channel_test tests/webview_channel_test.cpp)
  target_include_directories(webview_channel_test PRIVATE src)
  target_link_libraries(webview_channel_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.webview_channel COMMAND webview_channel_test)
  set_tests_properties(runtime.webview_channel PROPERTIES TIMEOUT 60)

  # The WebView request dispatch (ADR-0056 step a): APKRT_REQUEST_WEBVIEW to the host's webview handler.
  add_executable(webview_session_test tests/webview_session_test.cpp)
  target_include_directories(webview_session_test PRIVATE src)
  target_link_libraries(webview_session_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.webview_session COMMAND webview_session_test)
  set_tests_properties(runtime.webview_session PROPERTIES TIMEOUT 60)

  # The audio output streams (ADR-0036, decisions 5 and 6), with a fake host and, for the workgroup, the Mac's
  # default output unit.
  add_executable(audio_test tests/audio_test.cpp)
  target_include_directories(audio_test PRIVATE src)
  target_link_libraries(audio_test PRIVATE apkrt apk_defaults apk_testing "-framework AudioToolbox")
  add_test(NAME runtime.audio COMMAND audio_test)
  set_tests_properties(runtime.audio PROPERTIES TIMEOUT 60)

  # The audio input streams (ADR-0064), with a fake input host.
  add_executable(audio_input_test tests/audio_input_test.cpp)
  target_include_directories(audio_input_test PRIVATE src)
  target_link_libraries(audio_input_test PRIVATE apkrt apk_defaults apk_testing)
  add_test(NAME runtime.audio_input COMMAND audio_input_test)
  set_tests_properties(runtime.audio_input PROPERTIES TIMEOUT 60)
  add_executable(audio_effects_test tests/audio_effects_test.cpp)
  target_include_directories(audio_effects_test PRIVATE src)
  target_link_libraries(audio_effects_test PRIVATE apkrt apk_defaults)
  add_test(NAME runtime.audio_effects COMMAND audio_effects_test)
  add_executable(seq_words_test tests/seq_words_test.cpp)
  target_include_directories(seq_words_test PRIVATE src)
  target_link_libraries(seq_words_test PRIVATE apk_defaults)
  add_test(NAME runtime.seq_words COMMAND seq_words_test)

  # libaaudio's and libOpenSLES's own logic on the Mac (ADR-0036, decisions 6 and 7): runtime/android/aaudio.cpp and
  # opensles.cpp against the NDK's headers and tests/aaudio_host, with their syscalls going to the runtime's dispatcher.
  set(ndk_include "toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/include")
  set(aaudio_ndk "${APK_ANDROID_NDK}")
  if(NOT EXISTS "${aaudio_ndk}/${ndk_include}/aaudio/AAudio.h")
    set(aaudio_ndk "$ENV{HOME}/Library/Android/sdk/ndk/30.0.16248370")  # graphics/CMakeLists.txt's default
  endif()
  if(EXISTS "${aaudio_ndk}/${ndk_include}/aaudio/AAudio.h")
    set(host_include "${CMAKE_CURRENT_BINARY_DIR}/aaudio_host/include")
    file(COPY "${aaudio_ndk}/${ndk_include}/aaudio/AAudio.h" DESTINATION "${host_include}/aaudio")
    file(COPY "${aaudio_ndk}/${ndk_include}/SLES" "${aaudio_ndk}/${ndk_include}/jni.h" DESTINATION "${host_include}")
    set_source_files_properties(android/aaudio.cpp android/opensles.cpp PROPERTIES COMPILE_OPTIONS
      "-Dsyscall=minato_host_syscall;-Wno-deprecated-declarations")
    add_executable(aaudio_library_test tests/aaudio_library_test.cpp tests/aaudio_host/host.cpp android/aaudio.cpp)
    add_executable(opensl_library_test tests/opensl_library_test.cpp tests/aaudio_host/host.cpp android/aaudio.cpp
      android/opensles.cpp)
    add_executable(audio_matrix_test tests/audio_matrix_test.cpp tests/aaudio_host/host.cpp android/aaudio.cpp
      android/opensles.cpp)
    foreach(test IN ITEMS aaudio_library_test opensl_library_test audio_matrix_test)
      target_include_directories(${test} PRIVATE src tests/aaudio_host)
      target_include_directories(${test} SYSTEM PRIVATE "${host_include}")
      target_compile_options(${test} PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/tests/aaudio_host/shim.h")
      target_link_libraries(${test} PRIVATE apkrt apk_defaults apk_testing)
    endforeach()
    add_test(NAME runtime.aaudio_library COMMAND aaudio_library_test)
    add_test(NAME runtime.opensl_library COMMAND opensl_library_test)
    add_test(NAME runtime.audio_matrix COMMAND audio_matrix_test)
    set_tests_properties(runtime.aaudio_library runtime.opensl_library PROPERTIES TIMEOUT 60)
    set_tests_properties(runtime.audio_matrix PROPERTIES TIMEOUT 300)
  endif()

  add_executable(window_test tests/window_test.cpp tests/window_probe.S tests/blocking_probe.S tests/threads_probe.S)
  target_include_directories(window_test PRIVATE src)
  target_link_libraries(window_test PRIVATE apkrt apk_defaults apk_testing "-framework CoreFoundation")
  if(APK_SANITIZE)
    set_source_files_properties(tests/window_test.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
  endif()
  add_test(NAME runtime.window COMMAND window_test)
  set_tests_properties(runtime.window PROPERTIES TIMEOUT 120)

  # ADR-0028's test 3: a scripted host plays ADR-0032's lifecycle for activity-test through activity-host and
  # compares the app's log with Android 15's (fixtures/expected/activity-test.logcat).
  add_library(app_host STATIC tests/app_host.cpp)
  target_link_libraries(app_host PUBLIC apkrt apkconv apk_defaults)
  add_executable(activity_test tests/activity_test.cpp)
  target_link_libraries(activity_test PRIVATE app_host)
  # Its variants: an onStart that polls the main thread's looper, a host that leaves NATIVE_WINDOW_DESTROYED
  # out, an app that asks for a window format with alpha (WINDOW_OPAQUE 0), a set of two APKs (ADR-0048), and a
  # library, a function or an APK that is not there (START_FAILED, status 1).
  foreach(case IN ITEMS "" polling leftovers translucent split missing-library missing-function
                         missing-apk)
    set(name runtime.activity)
    if(case)
      set(name runtime.activity.${case})
    endif()
    add_test(NAME ${name} COMMAND activity_test "${CMAKE_CURRENT_BINARY_DIR}/activity_test_scratch"
      "${APK_FIXTURES_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/fixtures/expected"
      ${case})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120 SKIP_REGULAR_EXPRESSION "^skipped:")
  endforeach()
  # ADR-0036's choreographer-test: the same host plays the frame clock for choreographer-test and compares
  # its log with Android 15's (fixtures/expected/choreographer-test.logcat).
  add_executable(choreographer_test tests/choreographer_test.cpp)
  target_include_directories(choreographer_test PRIVATE src)
  target_link_libraries(choreographer_test PRIVATE app_host)
  add_test(NAME runtime.choreographer COMMAND choreographer_test "${CMAKE_CURRENT_BINARY_DIR}/choreographer_test_scratch"
    "${APK_FIXTURES_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/fixtures/expected")
  set_tests_properties(runtime.choreographer PROPERTIES TIMEOUT 120 SKIP_REGULAR_EXPRESSION "^skipped:")
  # ADR-0036's input-test: the same host plays the gestures of gestures.txt through an input channel and
  # compares the log with Android 15's (fixtures/expected/input-test.logcat).
  add_executable(input_test tests/input_test.cpp)
  target_link_libraries(input_test PRIVATE app_host)
  add_test(NAME runtime.input COMMAND input_test "${CMAKE_CURRENT_BINARY_DIR}/input_test_scratch"
    "${APK_FIXTURES_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/fixtures/expected"
    "${PROJECT_SOURCE_DIR}/fixtures/apps/input-test/gestures.txt")
  set_tests_properties(runtime.input PROPERTIES TIMEOUT 120 SKIP_REGULAR_EXPRESSION "^skipped:")
  # ADR-0067 step 2: the same run with the window's and the channel's render scale 0.5, as the App sets them for an
  # upscaled app: the app sees its half-size window and the same gestures at the same fractions of it.
  add_test(NAME runtime.input_scaled COMMAND input_test "${CMAKE_CURRENT_BINARY_DIR}/input_scaled_scratch"
    "${APK_FIXTURES_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/fixtures/expected"
    "${PROJECT_SOURCE_DIR}/fixtures/apps/input-test/gestures.txt" 0.5)
  set_tests_properties(runtime.input_scaled PROPERTIES TIMEOUT 120 SKIP_REGULAR_EXPRESSION "^skipped:")
  # ADR-0068 step 2e's native-ime-test: the same host posts a text, which a NativeActivity's view does not take, and the
  # hardware keyboard's keys, and compares the log with Android 15's (fixtures/expected/native-ime-test.logcat).
  add_executable(native_ime_test tests/native_ime_test.cpp)
  target_link_libraries(native_ime_test PRIVATE app_host)
  # Its variant "single" takes one event per looper callback and ends B's down in its predispatch.
  foreach(case IN ITEMS "" single)
    set(name runtime.native-ime)
    if(case)
      set(name runtime.native-ime.${case})
    endif()
    add_test(NAME ${name} COMMAND native_ime_test "${CMAKE_CURRENT_BINARY_DIR}/native_ime_test_scratch"
      "${APK_FIXTURES_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/fixtures/expected"
      "${PROJECT_SOURCE_DIR}/build/native-ime-test/native-ime-test.apk" ${case})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120 SKIP_REGULAR_EXPRESSION "^skipped:")
  endforeach()
  # ADR-0068 decision 5, step 4's gamepad-native-test: the same host plays a game controller (a device record, key
  # records and axes records) and compares the log with Android 15's (fixtures/expected/gamepad-native-test.logcat).
  add_executable(gamepad_native_test tests/gamepad_native_test.cpp)
  target_link_libraries(gamepad_native_test PRIVATE app_host)
  add_test(NAME runtime.gamepad-native COMMAND gamepad_native_test
    "${CMAKE_CURRENT_BINARY_DIR}/gamepad_native_test_scratch"
    "${APK_FIXTURES_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/fixtures/expected")
  set_tests_properties(runtime.gamepad-native PROPERTIES TIMEOUT 120 SKIP_REGULAR_EXPRESSION "^skipped:")
  # ADR-0068 decision 5, the mouse step's mouse-native-test: the same host plays a mouse (a device record and
  # apkrt_input_post_mouse's reports) and compares the log with Android 15's
  # (fixtures/expected/mouse-native-test.logcat).
  add_executable(mouse_native_test tests/mouse_native_test.cpp)
  target_link_libraries(mouse_native_test PRIVATE app_host)
  add_test(NAME runtime.mouse-native COMMAND mouse_native_test
    "${CMAKE_CURRENT_BINARY_DIR}/mouse_native_test_scratch"
    "${APK_FIXTURES_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/fixtures/expected")
  set_tests_properties(runtime.mouse-native PROPERTIES TIMEOUT 120 SKIP_REGULAR_EXPRESSION "^skipped:")
  # ADR-0038's test 3 (step c): the same host turns rotation-test to landscape and back as the App does, with the
  # window's new size, and compares the log with Android 15's (fixtures/expected/rotation-test.logcat).
  add_executable(rotation_test tests/rotation_test.cpp)
  target_link_libraries(rotation_test PRIVATE app_host)
  add_test(NAME runtime.rotation COMMAND rotation_test "${CMAKE_CURRENT_BINARY_DIR}/rotation_test_scratch"
    "${APK_FIXTURES_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/fixtures/expected")
  set_tests_properties(runtime.rotation PROPERTIES TIMEOUT 120 SKIP_REGULAR_EXPRESSION "^skipped:")
  # ADR-0036's aaudio-test and opensl-test, and sdl-audio-test (the audio speed of an SDL2 game): the same host plays
  # the audio host's side with a consumer that reads every stream every 5 ms, and compares the log with Android 15's
  # (fixtures/expected/<app>.logcat).
  add_executable(audio_app_test tests/audio_app_test.cpp)
  target_link_libraries(audio_app_test PRIVATE app_host)
  # The guest libraries these tests load must be those of this tree: a libaaudio.so from before a field was added to
  # apkrt_audio_config read the session from the stack and made the input tests flake. A test that needs them lists the
  # fixture (FIXTURES_REQUIRED guest_bionic) and stops with the command that builds them again when they are stale;
  # APK_SKIP_GUEST_FRESH=1 turns the check off.
  add_test(NAME runtime.guest_bionic_fresh COMMAND Python3::Interpreter
    "${PROJECT_SOURCE_DIR}/tools/check-guest-fresh.py" --only build/bionic "${PROJECT_SOURCE_DIR}")
  set_tests_properties(runtime.guest_bionic_fresh PROPERTIES FIXTURES_SETUP guest_bionic)
  foreach(app IN ITEMS aaudio opensl sdl-audio)
    add_test(NAME runtime.${app} COMMAND audio_app_test "${CMAKE_CURRENT_BINARY_DIR}/${app}_app_test_scratch"
      "${APK_FIXTURES_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/fixtures/expected"
      ${app}-test)
    set_tests_properties(runtime.${app} PROPERTIES TIMEOUT 120 SKIP_REGULAR_EXPRESSION "^skipped:"
      FIXTURES_REQUIRED guest_bionic)
  endforeach()

  # ADR-0064's audio-input-test, without and with RECORD_AUDIO's grant (the twin audio-input-test-granted): the same
  # host with a microphone that captures silence.
  foreach(app IN ITEMS audio-input-test audio-input-test-granted)
    add_test(NAME runtime.${app} COMMAND audio_app_test "${CMAKE_CURRENT_BINARY_DIR}/${app}_app_test_scratch"
      "${APK_FIXTURES_DIR}" "${APK_BIONIC_DIR}" "${APK_GUEST_SYSTEM_DIR}" "${PROJECT_SOURCE_DIR}/fixtures/expected"
      ${app})
    set_tests_properties(runtime.${app} PROPERTIES TIMEOUT 120 SKIP_REGULAR_EXPRESSION "^skipped:"
      FIXTURES_REQUIRED guest_bionic)
  endforeach()

  # Guest signal frames against the x18 islands (ADR-0020 X4), on clone children.
  add_executable(signals_test tests/signals_test.cpp tests/signals_probe.S tests/threads_probe.S)
  target_include_directories(signals_test PRIVATE src)
  target_link_libraries(signals_test PRIVATE apkrt apk_defaults apk_testing)
  if(APK_SANITIZE)
    set_source_files_properties(tests/signals_test.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
  endif()
  add_test(NAME runtime.signals COMMAND signals_test)
  set_tests_properties(runtime.signals PROPERTIES TIMEOUT 60)

  # Leaving x18 islands (ADR-0020 X4): a harness that single-steps calls into a converted program,
  # and clone children that get signals and faults inside its islands.
  add_executable(islands_test tests/islands_test.cpp tests/stepper.cpp tests/stepper_probe.S tests/threads_probe.S
    tests/signals_probe.S)
  target_include_directories(islands_test PRIVATE src "${PROJECT_SOURCE_DIR}/importer/converter/tests")
  target_link_libraries(islands_test PRIVATE apkrt apkconv apk_defaults apk_testing)
  if(APK_SANITIZE)
    set_source_files_properties(tests/islands_test.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
  endif()
  add_test(NAME runtime.islands COMMAND islands_test "${CMAKE_CURRENT_BINARY_DIR}/islands_test_scratch")
  set_tests_properties(runtime.islands PROPERTIES TIMEOUT 120)

  add_executable(syscall_entry_test tests/syscall_entry_test.c tests/syscall_entry_probe.S)
  target_link_libraries(syscall_entry_test PRIVATE apkrt apk_defaults)
  add_test(NAME runtime.syscall_entry COMMAND syscall_entry_test)
endif()
