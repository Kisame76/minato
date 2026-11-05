# The guest libc (ADR-0021): AOSP bionic android-15.0.0_r1 (tools/fetch-sources.py), built with the
# NDK's clang as Android arm64 platform code: ld-android.so (the stub libc.so links against),
# libdl.so, libdl_android.so (the linker's namespace functions, for ART: ADR-0033), libc.so and
# libm.so. Which sources and flags come from bionic's own Android.bp files
# (tools/bionic-plan.py). Built by tools/build-bionic.sh; the results go to <build>/lib.
cmake_minimum_required(VERSION 4.0)
project(MinatoBionic LANGUAGES C CXX ASM)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Build bionic with tools/build-bionic.sh (Android NDK, arm64-v8a).")
endif()

set(APK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(APK_SOURCES "${APK_ROOT}/build/sources" CACHE PATH "The fetched AOSP sources (tools/fetch-sources.py)")
set(APK_BIONIC_GENERATED "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${APK_BIONIC_GENERATED}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/archive")

# Stand-ins for projects we do not fetch: the liblog header from the NDK (only android/log.h, the
# NDK's other headers would shadow bionic's), LLVM's libunwind and the compiler builtins.
set(ndk_sysroot_include "${CMAKE_SYSROOT}/usr/include")
file(COPY "${ndk_sysroot_include}/android/log.h" DESTINATION "${CMAKE_BINARY_DIR}/ndk_liblog/include/android")
file(GLOB ndk_clang_libs "${ANDROID_TOOLCHAIN_ROOT}/lib/clang/*/lib/linux")
set(ndk_builtins "${ndk_clang_libs}/libclang_rt.builtins-aarch64-android.a")
set(ndk_libunwind "${ndk_clang_libs}/aarch64/libunwind.a")
set(ndk_crt "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-android/${ANDROID_PLATFORM_LEVEL}")
foreach(file IN ITEMS "${ndk_builtins}" "${ndk_libunwind}" "${ndk_crt}/crtbegin_so.o" "${ndk_crt}/crtend_so.o")
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "missing from the NDK: ${file}")
  endif()
endforeach()

# Android's "libunwind-exported": the NDK's libunwind with its hidden symbols made visible, so that
# libc.so can export _Unwind_Backtrace and friends as Android's does (LIBC_R).
set(libunwind_exported "${CMAKE_BINARY_DIR}/libunwind-exported.a")
add_custom_command(OUTPUT "${libunwind_exported}"
  COMMAND ${Python3_EXECUTABLE} "${APK_ROOT}/tools/export-archive-symbols.py" "${CMAKE_AR}" "${ndk_libunwind}"
    "${libunwind_exported}"
  DEPENDS "${ndk_libunwind}" "${APK_ROOT}/tools/export-archive-symbols.py" VERBATIM)
add_custom_target(libunwind_exported DEPENDS "${libunwind_exported}")

# Soong's global flags for Android arm64 platform code (build/soong/cc/config), as far as they
# matter here: every API is visible (android10000), only bionic's own headers (-nostdlibinc), 16 KiB
# pages (ADR-0007). -Wno-error comes last: bionic's -Werror was meant for the clang of its time.
# PLATFORM_SDK_VERSION is a Soong product variable: 35 for Android 15.
set(apk_target "--target=aarch64-linux-android10000")
set(apk_flags ${apk_target} -nostdlibinc -fPIC -ffunction-sections -fdata-sections -fno-short-enums
  -funwind-tables -fstack-protector-strong -fno-exceptions -fno-strict-aliasing -ffp-contract=off
  -ftrivial-auto-var-init=zero -march=armv8-a -O2 -DANDROID -DNDEBUG)
set(apk_link_flags ${apk_target} -nostdlib -Wl,--no-undefined -Wl,-z,max-page-size=16384 -Wl,-z,noexecstack
  -Wl,-z,relro -Wl,-z,now -Wl,--build-id=md5 -Wl,--hash-style=gnu)

# STRICT marks Minato's own units, whose warnings are errors: -Werror after bionic's -Wno-error.
function(apk_bionic_unit name)
  cmake_parse_arguments(PARSE_ARGV 1 unit "STRICT" "" "SOURCES;CFLAGS;CONLYFLAGS;CPPFLAGS;ASFLAGS;INCLUDES")
  add_library(${name} OBJECT ${unit_SOURCES})
  target_include_directories(${name} PRIVATE ${unit_INCLUDES} "${CMAKE_BINARY_DIR}/ndk_liblog/include")
  target_compile_options(${name} PRIVATE ${apk_flags}
    "$<$<COMPILE_LANGUAGE:C>:-std=gnu17;${unit_CFLAGS};${unit_CONLYFLAGS}>"
    "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-rtti;${unit_CFLAGS};${unit_CPPFLAGS}>"
    "$<$<COMPILE_LANGUAGE:ASM>:-D__ASSEMBLY__;-DPLATFORM_SDK_VERSION=35;${unit_ASFLAGS}>"
    -Wno-error)
  if(unit_STRICT)
    target_compile_options(${name} PRIVATE -Werror)
  endif()
endfunction()

function(apk_bionic_library name)
  cmake_parse_arguments(PARSE_ARGV 1 library "" "NOCRT" "UNITS;STATIC;NEEDED;SUBSTITUTES;LDFLAGS;VERSION_SCRIPTS")
  set(objects)
  foreach(unit IN LISTS library_UNITS)
    list(APPEND objects "$<TARGET_OBJECTS:${unit}>")
  endforeach()
  set(target guest_${name})
  add_library(${target} SHARED ${objects})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME ${name} PREFIX "" SUFFIX ".so" LINKER_LANGUAGE C)
  target_link_options(${target} PRIVATE ${apk_link_flags} ${library_LDFLAGS})
  if(library_VERSION_SCRIPTS)
    foreach(script IN LISTS library_VERSION_SCRIPTS)
      target_link_options(${target} PRIVATE "-Wl,--version-script=${script}")
      set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${script}")
    endforeach()
    # Version scripts can be generated: make the link wait for them.
    add_custom_target(${target}_version_scripts DEPENDS ${library_VERSION_SCRIPTS})
    add_dependencies(${target} ${target}_version_scripts)
  endif()
  if(NOT library_NOCRT)
    target_sources(${target} PRIVATE "${ndk_crt}/crtbegin_so.o")
  endif()
  foreach(static IN LISTS library_STATIC)
    string(MAKE_C_IDENTIFIER "bionic_${static}" unit)
    if(TARGET ${unit})
      if(NOT TARGET guest_static_${static})
        add_library(guest_static_${static} STATIC "$<TARGET_OBJECTS:${unit}>")
      endif()
      target_link_libraries(${target} PRIVATE guest_static_${static})
    endif()
  endforeach()
  foreach(needed IN LISTS library_NEEDED)
    target_link_libraries(${target} PRIVATE guest_${needed})
  endforeach()
  if("ndk_libunwind" IN_LIST library_SUBSTITUTES)
    add_dependencies(${target} libunwind_exported)
    target_link_libraries(${target} PRIVATE "-Wl,--whole-archive" "${libunwind_exported}" "-Wl,--no-whole-archive")
  endif()
  target_link_libraries(${target} PRIVATE "${ndk_builtins}")
  if(NOT library_NOCRT)
    target_link_libraries(${target} PRIVATE "${ndk_crt}/crtend_so.o")
  endif()
endfunction()

execute_process(COMMAND ${Python3_EXECUTABLE} "${APK_ROOT}/tools/bionic-plan.py" "${APK_SOURCES}" "${CMAKE_BINARY_DIR}/plan.cmake"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "tools/bionic-plan.py failed")
endif()
# A change to the planner plans again. So does a file that comes into the overlay or leaves it, since the plan names
# the overlay's file in place of bionic's; a changed one only compiles again.
file(GLOB_RECURSE bionic_overlay CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/overlay/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${APK_ROOT}/tools/bionic-plan.py"
  "${APK_ROOT}/tools/blueprint.py")
include("${CMAKE_BINARY_DIR}/plan.cmake")

# ld-android.so has no libc, because libc.so needs it. Its lock's atomics stay inline: the
# compiler's outline atomics set themselves up with getauxval and system properties from libc.so.
target_compile_options(bionic_ld_android PRIVATE -mno-outline-atomics)

# Scudo with our copy of its configuration (ADR-0035): a region of 128 MiB per size class, reserved when the class is
# first used, instead of 8.25 GiB in one piece at the first malloc. The folder comes before AOSP's config/, so that
# Scudo's #include "custom_scudo_config.h" finds our copy.
target_include_directories(bionic_libscudo BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/scudo")

# A second guest libc, libc-bigregions.so: identical to libc.so but with scudo's 256 MiB regions (RegionSizeLog 28 vs
# 27, ADR-0035 item 3). The loader maps /system/lib64/libc.so to whichever the process's address space allows. Only
# the scudo objects differ -- one -D -- so recompile just those (bionic_libscudo_big) and relink libc with them swapped
# in, reusing guest_libc's own recipe (sources, link options and needed libraries) so the two stay in step.
get_target_property(scudo_sources bionic_libscudo SOURCES)
get_target_property(scudo_includes bionic_libscudo INCLUDE_DIRECTORIES)
get_target_property(scudo_options bionic_libscudo COMPILE_OPTIONS)
add_library(bionic_libscudo_big OBJECT ${scudo_sources})
target_include_directories(bionic_libscudo_big PRIVATE ${scudo_includes})
target_compile_options(bionic_libscudo_big PRIVATE ${scudo_options} -DMINATO_SCUDO_REGION_SIZE_LOG=28U)

get_target_property(libc_sources guest_libc SOURCES)
string(REPLACE "$<TARGET_OBJECTS:bionic_libscudo>" "$<TARGET_OBJECTS:bionic_libscudo_big>" libc_big_sources
  "${libc_sources}")
add_library(guest_libc_big SHARED ${libc_big_sources})
# NO_SONAME stops CMake adding -soname=libc-bigregions.so from the output name; the explicit -soname below then makes
# the SONAME libc.so, because the guest's DT_NEEDED names libc.so and the loader swaps in this file (dl.cpp) as a
# drop-in, so its identity to the guest linker must still be libc.so.
set_target_properties(guest_libc_big PROPERTIES OUTPUT_NAME libc-bigregions PREFIX "" SUFFIX ".so" LINKER_LANGUAGE C
  NO_SONAME TRUE)
target_link_options(guest_libc_big PRIVATE "-Wl,-soname,libc.so")
get_target_property(libc_big_link_options guest_libc LINK_OPTIONS)
if(libc_big_link_options)
  target_link_options(guest_libc_big PRIVATE ${libc_big_link_options})
endif()
get_target_property(libc_big_link_libraries guest_libc LINK_LIBRARIES)
if(libc_big_link_libraries)
  target_link_libraries(guest_libc_big PRIVATE ${libc_big_link_libraries})
endif()
# Build after guest_libc so any generated version script is already there, and it shares guest_libc's dependencies.
add_dependencies(guest_libc_big guest_libc)

# liblog.so: Minato's own (runtime/bionic/src/liblog.cpp), because Android's needs the logd
# service. The NDK's API without symbol versions, like the NDK's stub library.
set(bionic_include "${APK_SOURCES}/bionic/libc/include" "${APK_SOURCES}/bionic/libc/kernel/uapi"
  "${APK_SOURCES}/bionic/libc/kernel/android/uapi" "${APK_SOURCES}/bionic/libc/kernel/uapi/asm-arm64")
apk_bionic_unit(apk_liblog SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/liblog.cpp" INCLUDES ${bionic_include}
  CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(liblog UNITS apk_liblog NEEDED libc)

# libnetd_client.so: Minato's own (runtime/bionic/src/libnetd_client.cpp). bionic's libc.so opens
# it at start; as Android's, it refuses AF_INET sockets without networking and opens the resolver's proxy.
apk_bionic_unit(apk_netd_client SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/libnetd_client.cpp" INCLUDES ${bionic_include}
  CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(libnetd_client UNITS apk_netd_client NEEDED libc)

# The NDK libraries Minato writes itself (ADR-0028, decisions 7, 9 and 12, runtime/android): libz.so,
# libnativewindow.so, libandroid.so, libaaudio.so and libOpenSLES.so (ADR-0036). Each exports exactly the names of
# its NDK stub, with a version script that runtime/android/stubgen.py writes from the stub, and a generated stub for
# every function not written by hand. Only the NDK headers of the APIs they implement are copied, since the
# NDK's other headers would shadow bionic's.
set(android_sources "${CMAKE_CURRENT_SOURCE_DIR}/../android")
set(android_generated "${APK_BIONIC_GENERATED}/android")
file(MAKE_DIRECTORY "${android_generated}")
foreach(header IN ITEMS asset_manager choreographer configuration data_space hardware_buffer input keycodes looper
    native_activity native_window native_window_jni rect sensor sharedmem sharedmem_jni surface_texture window)
  file(COPY "${ndk_sysroot_include}/android/${header}.h" DESTINATION "${CMAKE_BINARY_DIR}/ndk_android/include/android")
endforeach()
foreach(header IN ITEMS jni zconf zlib)
  file(COPY "${ndk_sysroot_include}/${header}.h" DESTINATION "${CMAKE_BINARY_DIR}/ndk_android/include")
endforeach()
file(COPY "${ndk_sysroot_include}/aaudio/AAudio.h" DESTINATION "${CMAKE_BINARY_DIR}/ndk_android/include/aaudio")
file(COPY "${ndk_sysroot_include}/camera" DESTINATION "${CMAKE_BINARY_DIR}/ndk_android/include")
foreach(header IN ITEMS NdkImage NdkImageReader NdkMediaCodec NdkMediaCrypto NdkMediaDataSource NdkMediaError
                       NdkMediaExtractor NdkMediaFormat)
  file(COPY "${ndk_sysroot_include}/media/${header}.h" DESTINATION "${CMAKE_BINARY_DIR}/ndk_android/include/media")
endforeach()
file(COPY "${ndk_sysroot_include}/OMXAL" DESTINATION "${CMAKE_BINARY_DIR}/ndk_android/include")
foreach(header IN ITEMS OpenSLES OpenSLES_Android OpenSLES_AndroidConfiguration OpenSLES_AndroidMetadata
    OpenSLES_Platform)
  file(COPY "${ndk_sysroot_include}/SLES/${header}.h" DESTINATION "${CMAKE_BINARY_DIR}/ndk_android/include/SLES")
endforeach()
set(android_includes ${bionic_include} "${CMAKE_BINARY_DIR}/ndk_android/include" "${APK_ROOT}/runtime/include")
set(stubgen ${Python3_EXECUTABLE} "${android_sources}/stubgen.py" generate)
set(stubgen_tools --sysroot "${CMAKE_SYSROOT}" --clang "${CMAKE_C_COMPILER}" --nm "${ANDROID_TOOLCHAIN_ROOT}/bin/llvm-nm")
set(ndk_stubs "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-android/35")

# libz.so: the NDK's libz.a, zlib as AOSP ships it, linked whole (decision 12). The archive hides the
# internal names that the stub lists (_tr_init and others): llvm-objcopy gives every name of the stub
# default visibility, and -Bsymbolic keeps the archive's own references to them direct, as compiled.
set(libz_map "${android_generated}/libz.map")
set(libz_names "${android_generated}/libz.names")
set(libz_exported "${CMAKE_BINARY_DIR}/libz-exported.a")
set(ndk_libz "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-android/libz.a")
add_custom_command(OUTPUT "${libz_map}" "${libz_names}"
  COMMAND ${stubgen} libz ${stubgen_tools} --map "${libz_map}" --names "${libz_names}"
  DEPENDS "${android_sources}/stubgen.py" "${ndk_stubs}/libz.so" VERBATIM)
add_custom_command(OUTPUT "${libz_exported}"
  COMMAND "${ANDROID_TOOLCHAIN_ROOT}/bin/llvm-objcopy" "--set-symbols-visibility=${libz_names}=default"
    "${ndk_libz}" "${libz_exported}"
  DEPENDS "${ndk_libz}" "${libz_names}" VERBATIM)
add_custom_target(libz_exported DEPENDS "${libz_exported}")
apk_bionic_library(libz NEEDED libc LDFLAGS -Wl,-Bsymbolic VERSION_SCRIPTS "${libz_map}")
add_dependencies(guest_libz libz_exported)
target_link_libraries(guest_libz PRIVATE "-Wl,--whole-archive" "${libz_exported}" "-Wl,--no-whole-archive")

# libnativewindow.so: the window (runtime/android/native_window.cpp) and AHardwareBuffer (hardware_buffer.cpp,
# ADR-0058 a)), version LIBNATIVEWINDOW.
set(nativewindow_map "${android_generated}/libnativewindow.map")
set(nativewindow_stubs "${android_generated}/libnativewindow_stubs.c")
add_custom_command(OUTPUT "${nativewindow_map}" "${nativewindow_stubs}"
  COMMAND ${stubgen} libnativewindow ${stubgen_tools} --map "${nativewindow_map}" --stubs "${nativewindow_stubs}"
  DEPENDS "${android_sources}/stubgen.py" "${ndk_stubs}/libnativewindow.so" VERBATIM)
apk_bionic_unit(apk_nativewindow SOURCES "${android_sources}/native_window.cpp"
  "${android_sources}/hardware_buffer.cpp" "${nativewindow_stubs}"
  INCLUDES ${android_includes} CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(libnativewindow UNITS apk_nativewindow NEEDED libc liblog VERSION_SCRIPTS "${nativewindow_map}")

# libandroid.so: ALooper, AConfiguration, the asset manager, ANativeActivity's requests, the input queue and
# its motion events, AChoreographer, the forwards to libnativewindow, a Java Surface's window through
# libandroid_runtime, and ASharedMemory on memfd_create (runtime/android).
set(android_map "${android_generated}/libandroid.map")
set(android_stubs "${android_generated}/libandroid_stubs.c")
add_custom_command(OUTPUT "${android_map}" "${android_stubs}"
  COMMAND ${stubgen} libandroid ${stubgen_tools} --map "${android_map}" --stubs "${android_stubs}"
  DEPENDS "${android_sources}/stubgen.py" "${ndk_stubs}/libandroid.so" VERBATIM)
apk_bionic_unit(apk_android SOURCES "${android_sources}/looper.cpp" "${android_sources}/configuration.cpp"
  "${android_sources}/asset_manager.cpp" "${android_sources}/native_activity.cpp" "${android_sources}/input_queue.cpp"
  "${android_sources}/input.cpp" "${android_sources}/choreographer.cpp" "${android_sources}/window_forwards.cpp"
  "${android_sources}/hardware_buffer_forwards.cpp" "${android_sources}/hardware_buffer_jni.cpp"
  "${android_sources}/native_window_jni.cpp" "${android_sources}/sharedmem.cpp" "${android_sources}/sensor_manager.c"
  "${android_sources}/surface_texture.cpp"
  "${android_stubs}"
  INCLUDES ${android_includes} CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(libandroid UNITS apk_android NEEDED libc libdl liblog libnativewindow libz
  VERSION_SCRIPTS "${android_map}")

# libaaudio.so: AAudio's output streams on the runtime's (runtime/android/aaudio.cpp, ADR-0036, decision 6), every
# name written by hand, without versions like the NDK's stub.
set(aaudio_map "${android_generated}/libaaudio.map")
add_custom_command(OUTPUT "${aaudio_map}"
  COMMAND ${stubgen} libaaudio ${stubgen_tools} --map "${aaudio_map}"
  DEPENDS "${android_sources}/stubgen.py" "${ndk_stubs}/libaaudio.so" VERBATIM)
apk_bionic_unit(apk_aaudio SOURCES "${android_sources}/aaudio.cpp" INCLUDES ${android_includes} CFLAGS -Wall -Wextra
  STRICT)
apk_bionic_library(libaaudio UNITS apk_aaudio NEEDED libc liblog VERSION_SCRIPTS "${aaudio_map}")

# libOpenSLES.so: OpenSL ES's engine, output mix and buffer-queue players on libaaudio's streams
# (runtime/android/opensles.cpp, ADR-0036, decision 7), every name written by hand, without versions like the NDK's
# stub.
set(opensles_map "${android_generated}/libOpenSLES.map")
add_custom_command(OUTPUT "${opensles_map}"
  COMMAND ${stubgen} libOpenSLES ${stubgen_tools} --map "${opensles_map}"
  DEPENDS "${android_sources}/stubgen.py" "${ndk_stubs}/libOpenSLES.so" VERBATIM)
apk_bionic_unit(apk_opensles SOURCES "${android_sources}/opensles.cpp" INCLUDES ${android_includes}
  CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(libOpenSLES UNITS apk_opensles NEEDED libc libm liblog libaaudio VERSION_SCRIPTS "${opensles_map}")

# libcamera2ndk.so and libmediandk.so (ADR-0042 b, gap 15): no camera and no media codecs, so that an app that
# links them, as Godot's libgodot_android.so does, loads. The camera manager has no camera
# (runtime/android/camera_manager.c); libmediandk's AImageReader and AImage are on libnativewindow's buffer queue
# (runtime/android/image_reader.cpp, ADR-0058 e1)), and its key strings have Android 15's values
# (runtime/android/media_keys.cpp, ADR-0060 e2b), and its AMediaExtractor reads mp4 files through Minato's parser
# (media_extractor.cpp and mp4_extractor.cpp, e2d); every other function is a generated stub that answers its API's
# error.
set(camera2ndk_map "${android_generated}/libcamera2ndk.map")
set(camera2ndk_stubs "${android_generated}/libcamera2ndk_stubs.c")
add_custom_command(OUTPUT "${camera2ndk_map}" "${camera2ndk_stubs}"
  COMMAND ${stubgen} libcamera2ndk ${stubgen_tools} --map "${camera2ndk_map}" --stubs "${camera2ndk_stubs}"
  DEPENDS "${android_sources}/stubgen.py" "${ndk_stubs}/libcamera2ndk.so" VERBATIM)
apk_bionic_unit(apk_camera2ndk SOURCES "${android_sources}/camera_manager.c" "${camera2ndk_stubs}"
  INCLUDES ${android_includes} CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(libcamera2ndk UNITS apk_camera2ndk NEEDED libc liblog VERSION_SCRIPTS "${camera2ndk_map}")
set(mediandk_map "${android_generated}/libmediandk.map")
set(mediandk_stubs "${android_generated}/libmediandk_stubs.c")
add_custom_command(OUTPUT "${mediandk_map}" "${mediandk_stubs}"
  COMMAND ${stubgen} libmediandk ${stubgen_tools} --map "${mediandk_map}" --stubs "${mediandk_stubs}"
  DEPENDS "${android_sources}/stubgen.py" "${ndk_stubs}/libmediandk.so" VERBATIM)
apk_bionic_unit(apk_mediandk SOURCES "${android_sources}/image_reader.cpp" "${android_sources}/media_keys.cpp"
  "${android_sources}/media_format.cpp" "${android_sources}/media_codec.cpp" "${android_sources}/mp4_extractor.cpp"
  "${android_sources}/media_extractor.cpp" "${android_sources}/ogg_extractor.cpp" "${mediandk_stubs}"
  INCLUDES ${android_includes} CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(libmediandk UNITS apk_mediandk NEEDED libc libdl liblog libm libnativewindow
  VERSION_SCRIPTS "${mediandk_map}")

# libsync.so, libamidi.so, libbinder_ndk.so, libneuralnetworks.so and libOpenMAXAL.so (the compatibility plan's gap 14):
# nothing stands behind them in Minato (no sync files, MIDI devices, Binder, NNAPI driver or OpenMAX AL engine), so that
# an app that names one in NEEDED loads, and every function answers its API's error, as it does on a phone without
# the service. The generated stubs log their name once; neuralnetworks.c (the device count 0) and openmaxal.c (the
# interface IDs and an engine that cannot be made) write through their arguments and are by hand.
foreach(library IN ITEMS libsync libamidi libbinder_ndk)
  set(${library}_map "${android_generated}/${library}.map")
  set(${library}_stubs "${android_generated}/${library}_stubs.c")
  add_custom_command(OUTPUT "${${library}_map}" "${${library}_stubs}"
    COMMAND ${stubgen} ${library} ${stubgen_tools} --map "${${library}_map}" --stubs "${${library}_stubs}"
    DEPENDS "${android_sources}/stubgen.py" "${ndk_stubs}/${library}.so" VERBATIM)
  apk_bionic_unit(apk_${library} SOURCES "${${library}_stubs}" INCLUDES ${android_includes} CFLAGS -Wall -Wextra STRICT)
  apk_bionic_library(${library} UNITS apk_${library} NEEDED libc liblog VERSION_SCRIPTS "${${library}_map}")
endforeach()
set(neuralnetworks_map "${android_generated}/libneuralnetworks.map")
set(neuralnetworks_stubs "${android_generated}/libneuralnetworks_stubs.c")
add_custom_command(OUTPUT "${neuralnetworks_map}" "${neuralnetworks_stubs}"
  COMMAND ${stubgen} libneuralnetworks ${stubgen_tools} --map "${neuralnetworks_map}" --stubs "${neuralnetworks_stubs}"
  DEPENDS "${android_sources}/stubgen.py" "${ndk_stubs}/libneuralnetworks.so" VERBATIM)
apk_bionic_unit(apk_neuralnetworks SOURCES "${android_sources}/neuralnetworks.c" "${neuralnetworks_stubs}"
  INCLUDES ${android_includes} CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(libneuralnetworks UNITS apk_neuralnetworks NEEDED libc liblog
  VERSION_SCRIPTS "${neuralnetworks_map}")
set(openmaxal_map "${android_generated}/libOpenMAXAL.map")
add_custom_command(OUTPUT "${openmaxal_map}"
  COMMAND ${stubgen} libOpenMAXAL ${stubgen_tools} --map "${openmaxal_map}"
  DEPENDS "${android_sources}/stubgen.py" "${ndk_stubs}/libOpenMAXAL.so" VERBATIM)
apk_bionic_unit(apk_openmaxal SOURCES "${android_sources}/openmaxal.c" INCLUDES ${android_includes}
  CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(libOpenMAXAL UNITS apk_openmaxal NEEDED libc VERSION_SCRIPTS "${openmaxal_map}")

# libminato-frame-rate.so (ADR-0066): the FPS unlock's wrappers of il2cpp_add_internal_call and
# mono_add_internal_call, which the loader preloads only with an unlock (runtime/android/frame_rate.c).
apk_bionic_unit(apk_frame_rate SOURCES "${android_sources}/frame_rate.c" INCLUDES ${android_includes}
  CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(libminato-frame-rate UNITS apk_frame_rate NEEDED libc)

# libminato-luajit.so (the LuaJIT switch): the wrapper of luaL_openlibs that switches LuaJIT's engine off after the
# libraries open, which the loader preloads when it is among the system's libraries (runtime/android/luajit.c).
apk_bionic_unit(apk_luajit SOURCES "${android_sources}/luajit.c" INCLUDES ${android_includes}
  CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(libminato-luajit UNITS apk_luajit NEEDED libc)

# libminato-ffi.so (libffi closures without executable memory, ADR-0072): a pool of trampolines and the wrappers of
# ffi_closure_alloc, ffi_prep_closure_loc and ffi_closure_free, which the loader preloads when it is among the system's
# libraries (runtime/android/ffi.c).
apk_bionic_unit(apk_ffi SOURCES "${android_sources}/ffi.c" INCLUDES ${android_includes}
  CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(libminato-ffi UNITS apk_ffi NEEDED libc)

# libminato-jsc.so (the JavaScriptCore switch): the function that sets JSC_useJIT=0 and JavaScriptCoreUseJIT=0, which
# the loader calls before the constructors of a library that defines JSGlobalContextCreate (runtime/android/jsc.c).
apk_bionic_unit(apk_jsc SOURCES "${android_sources}/jsc.c" INCLUDES ${android_includes}
  CFLAGS -Wall -Wextra STRICT)
apk_bionic_library(libminato-jsc UNITS apk_jsc NEEDED libc)

# activity-host (ADR-0028, decision 1): the guest program /system/bin/activity-host, which plays Android's app
# process for a NativeActivity (runtime/activity-host): the session's events become the app's callbacks, with
# libandroid's asset manager and the JNI stub. A PIE with linker64 as its interpreter, like the NDK's
# programs, into <build>/bin.
set(activity_host_sources "${CMAKE_CURRENT_SOURCE_DIR}/../activity-host")
apk_bionic_unit(apk_activity_host SOURCES "${activity_host_sources}/activity_host.cpp"
  "${activity_host_sources}/jni_stub.cpp" INCLUDES ${android_includes} CFLAGS -Wall -Wextra STRICT)
add_executable(guest_activity_host "${ndk_crt}/crtbegin_dynamic.o" "$<TARGET_OBJECTS:apk_activity_host>")
set_target_properties(guest_activity_host PROPERTIES OUTPUT_NAME activity-host LINKER_LANGUAGE C
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_options(guest_activity_host PRIVATE ${apk_link_flags} -pie -Wl,-dynamic-linker,/system/bin/linker64)
target_link_libraries(guest_activity_host PRIVATE guest_libandroid guest_libnativewindow guest_liblog guest_libdl
  guest_libc "${ndk_builtins}" "${ndk_crt}/crtend_android.o")

# The guest's /system/etc/public.libraries.txt (ADR-0040 decision 3), which libnativeloader reads when ART starts
# (ADR-0033 b), into <build>/system next to the time zone file of tools/build-bionic.sh.
set(public_libraries "${CMAKE_BINARY_DIR}/system/etc/public.libraries.txt")
add_custom_command(OUTPUT "${public_libraries}"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/system/etc/public.libraries.txt" "${public_libraries}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/system/etc/public.libraries.txt" VERBATIM)
add_custom_target(guest_public_libraries ALL DEPENDS "${public_libraries}")

# The guest's /system/etc/hosts, Android's two lines (system/core/rootdir/etc/hosts), which the resolver reads before
# DNS: the runtime's /dev/socket/dnsproxyd, and bionic itself in a process without networking (ADR-0054 step b).
set(guest_hosts "${CMAKE_BINARY_DIR}/system/etc/hosts")
add_custom_command(OUTPUT "${guest_hosts}"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/system/etc/hosts" "${guest_hosts}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/system/etc/hosts" VERBATIM)
add_custom_target(guest_hosts ALL DEPENDS "${guest_hosts}")

include("${APK_ROOT}/graphics/fonts/fonts.cmake")
