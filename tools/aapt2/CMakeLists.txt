# The host aapt2 (ADR-0039 b1): AOSP's aapt2 from build/sources/frameworks_base/tools/aapt2, as its Android.bp
# builds it (libaapt2, Main.cpp and the commands), with the libraries it links, each from its own Android.bp's
# sources for a darwin host: protobuf (libprotobuf-cpp-full, and aprotoc for aapt2's .proto files), libpng, expat,
# libziparchive, libutils, libcutils, libincfs-utils, libbuildversion and libandroidfw (STATIC_ANDROIDFW_FOR_TOOLS),
# libbase with fmtlib, and tools/aidl/liblog_host.cpp in place of liblog. zlib is the Mac's. Tests, fuzzers and
# gtest stay out. tools/build-aapt2.sh configures and builds it into build/aapt2.
cmake_minimum_required(VERSION 3.24)
project(minato_aapt2 LANGUAGES C CXX)

get_filename_component(MINATO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SOURCES "${MINATO_ROOT}/build/sources")
set(FRAMEWORKS "${SOURCES}/frameworks_base")
foreach(source frameworks_base protobuf libpng system_core_framework incremental_delivery build_soong libbase fmtlib
               libziparchive expat logging)
  if(NOT EXISTS "${SOURCES}/${source}")
    message(FATAL_ERROR "No ${SOURCES}/${source}: run tools/fetch-sources.py --set framework and --set art")
  endif()
endforeach()

include(soong.cmake)
find_package(ZLIB REQUIRED)

# The NDK headers libandroidfw's ResourceTypes.h includes, android/configuration.h and the asset_manager.h it
# includes, alone: AOSP takes them from frameworks/native's headers, which the NDK ships unchanged [inferred]; the
# whole NDK sysroot would bring Bionic's libc headers to the Mac.
set(NDK "$ENV{ANDROID_NDK}")
if(NOT NDK)
  set(NDK "$ENV{HOME}/Library/Android/sdk/ndk/30.0.16248370")
endif()
set(ndk_android "${NDK}/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/include/android")
set(ndk_headers "${CMAKE_CURRENT_BINARY_DIR}/ndk-headers")
file(MAKE_DIRECTORY "${ndk_headers}/android")
foreach(header configuration.h asset_manager.h)
  if(NOT EXISTS "${ndk_android}/${header}")
    message(FATAL_ERROR "No ${ndk_android}/${header}: set ANDROID_NDK to the NDK's folder")
  endif()
  file(CREATE_LINK "${ndk_android}/${header}" "${ndk_headers}/android/${header}" SYMBOLIC)
endforeach()

# Prepends `directory` to each name in the list `variable`.
function(sources_in variable directory)
  set(result "")
  foreach(name ${ARGN})
    list(APPEND result "${directory}/${name}")
  endforeach()
  set(${variable} "${result}" PARENT_SCOPE)
endfunction()

# --- liblog: the host shim of tools/aidl.
add_library(log STATIC "${MINATO_ROOT}/tools/aidl/liblog_host.cpp")
target_include_directories(log PUBLIC "${SOURCES}/logging/liblog/include")
soong_module(log FLAGS -Werror)

# --- libbase with fmtlib, as tools/aidl builds it.
sources_in(base_sources "${SOURCES}/libbase"
  abi_compatibility.cpp chrono_utils.cpp cmsg.cpp errors_unix.cpp file.cpp hex.cpp logging.cpp mapped_file.cpp
  parsebool.cpp parsenetaddress.cpp posix_strerror_r.cpp process.cpp properties.cpp result.cpp stringprintf.cpp
  strings.cpp threads.cpp)
add_library(base STATIC ${base_sources} "${SOURCES}/fmtlib/src/format.cc")
target_include_directories(base PUBLIC "${SOURCES}/libbase/include" "${SOURCES}/fmtlib/include")
# libbase_cflags_defaults. fmt's format strings are checked at run time, as in tools/aidl and runtime/art: fmt 10.2's
# compile-time check fails under the Mac's clang.
soong_module(base FLAGS -Wall -Werror -Wextra -Wexit-time-destructors)
target_compile_definitions(base PUBLIC _FILE_OFFSET_BITS=64 FMT_CONSTEVAL=)
target_link_libraries(base PUBLIC log)

# --- protobuf 3.21.12: libprotobuf-cpp-lite and -full, then libprotoc and aprotoc.
set(protobuf "${SOURCES}/protobuf/src/google/protobuf")
sources_in(protobuf_sources "${protobuf}"
  any_lite.cc arena.cc arenastring.cc arenaz_sampler.cc extension_set.cc generated_enum_util.cc
  generated_message_tctable_lite.cc generated_message_util.cc implicit_weak_message.cc inlined_string_field.cc
  io/coded_stream.cc io/io_win32.cc io/strtod.cc io/zero_copy_stream.cc io/zero_copy_stream_impl.cc
  io/zero_copy_stream_impl_lite.cc map.cc message_lite.cc parse_context.cc repeated_field.cc repeated_ptr_field.cc
  stubs/bytestream.cc stubs/common.cc stubs/int128.cc stubs/status.cc stubs/statusor.cc stubs/stringpiece.cc
  stubs/stringprintf.cc stubs/structurally_valid.cc stubs/strutil.cc stubs/time.cc wire_format_lite.cc
  any.cc any.pb.cc api.pb.cc compiler/importer.cc compiler/parser.cc descriptor.cc descriptor.pb.cc
  descriptor_database.cc duration.pb.cc dynamic_message.cc empty.pb.cc extension_set_heavy.cc field_mask.pb.cc
  generated_message_bases.cc generated_message_reflection.cc generated_message_tctable_full.cc io/gzip_stream.cc
  io/printer.cc io/tokenizer.cc map_field.cc message.cc reflection_ops.cc service.cc source_context.pb.cc
  struct.pb.cc stubs/substitute.cc text_format.cc timestamp.pb.cc type.pb.cc unknown_field_set.cc
  util/delimited_message_util.cc util/field_comparator.cc util/field_mask_util.cc util/internal/datapiece.cc
  util/internal/default_value_objectwriter.cc util/internal/error_listener.cc util/internal/field_mask_utility.cc
  util/internal/json_escaping.cc util/internal/json_objectwriter.cc util/internal/json_stream_parser.cc
  util/internal/object_writer.cc util/internal/proto_writer.cc util/internal/protostream_objectsource.cc
  util/internal/protostream_objectwriter.cc util/internal/type_info.cc util/internal/utility.cc util/json_util.cc
  util/message_differencer.cc util/time_util.cc util/type_resolver_util.cc wire_format.cc wrappers.pb.cc)
add_library(protobuf STATIC ${protobuf_sources})
# tools/aapt2/protobuf holds the config.h that stubs/common.cc includes; AOSP's android/config.h defines nothing
# that the sources read.
target_include_directories(protobuf PUBLIC "${SOURCES}/protobuf/src" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/protobuf")
# protobuf-cflags-defaults
set(protobuf_flags -Wall -Werror -Wno-missing-field-initializers -Wno-unused-function -Wno-unused-parameter
  -Wno-error=user-defined-warnings)
soong_module(protobuf EXTERNAL FLAGS ${protobuf_flags})
target_compile_definitions(protobuf PRIVATE HAVE_ZLIB=1)
target_link_libraries(protobuf PUBLIC ZLIB::ZLIB)

sources_in(protoc_sources "${protobuf}/compiler"
  code_generator.cc command_line_interface.cc cpp/enum.cc cpp/enum_field.cc cpp/extension.cc cpp/field.cc cpp/file.cc
  cpp/generator.cc cpp/helpers.cc cpp/map_field.cc cpp/message.cc cpp/message_field.cc cpp/padding_optimizer.cc
  cpp/parse_function_generator.cc cpp/primitive_field.cc cpp/service.cc cpp/string_field.cc plugin.cc plugin.pb.cc
  subprocess.cc zip_writer.cc)
# Only the C++ generator: tools/aapt2/protobuf/protoc_main.cc registers it alone, so aprotoc's other languages stay out.
add_executable(protoc ${protoc_sources} "${CMAKE_CURRENT_SOURCE_DIR}/protobuf/protoc_main.cc")
soong_module(protoc EXTERNAL FLAGS ${protobuf_flags} -Wno-unused-private-field)
target_link_libraries(protoc PRIVATE protobuf)

# --- libpng, with its arm64 NEON code but not filter_neon.S, as for arm64 in its Android.bp.
file(GLOB png_sources "${SOURCES}/libpng/*.c")
list(REMOVE_ITEM png_sources "${SOURCES}/libpng/example.c" "${SOURCES}/libpng/pngtest.c")
sources_in(png_arm "${SOURCES}/libpng/arm" arm_init.c filter_neon_intrinsics.c palette_neon_intrinsics.c)
add_library(png STATIC ${png_sources} ${png_arm})
target_include_directories(png PUBLIC "${SOURCES}/libpng")
soong_module(png EXTERNAL FLAGS -std=gnu89 -Wall -Werror -Wno-unused-parameter -O3)
target_link_libraries(png PUBLIC ZLIB::ZLIB)

# --- expat
sources_in(expat_sources "${SOURCES}/expat/expat/lib" xmlparse.c xmlrole.c xmltok.c)
add_library(expat STATIC ${expat_sources})
target_include_directories(expat PUBLIC "${SOURCES}/expat/lib" PRIVATE "${SOURCES}/expat")
soong_module(expat EXTERNAL FLAGS -fno-strict-aliasing -Wall -Werror -Wmissing-prototypes -Wstrict-prototypes
  -include stdio.h -fno-common)
target_compile_definitions(expat PRIVATE HAVE_EXPAT_CONFIG_H)

# --- libcutils and libutils (with libutils_binder) for a non-Windows, non-Linux host.
set(core "${SOURCES}/system_core_framework")
sources_in(cutils_sources "${core}/libcutils"
  config_utils.cpp iosched_policy.cpp load_file.cpp native_handle.cpp properties.cpp record_stream.cpp strlcpy.c
  fs.cpp hashmap.cpp multiuser.cpp str_parms.cpp ashmem-host.cpp trace-host.cpp)
add_library(cutils STATIC ${cutils_sources})
target_include_directories(cutils PUBLIC "${core}/libcutils/include" "${core}/libsystem/include"
                           PRIVATE "${core}/libutils/include")
soong_module(cutils FLAGS -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(cutils PUBLIC base log)

sources_in(utils_sources "${core}/libutils"
  FileMap.cpp JenkinsHash.cpp LightRefBase.cpp NativeHandle.cpp Printer.cpp StopWatch.cpp SystemClock.cpp Threads.cpp
  Timers.cpp Tokenizer.cpp misc.cpp)
sources_in(utils_binder_sources "${core}/libutils/binder"
  Errors.cpp RefBase.cpp SharedBuffer.cpp String16.cpp String8.cpp StrongPointer.cpp Unicode.cpp VectorImpl.cpp)
add_library(utils STATIC ${utils_sources} ${utils_binder_sources})
target_include_directories(utils PUBLIC "${core}/libutils/include" "${core}/libutils/binder/include")
soong_module(utils FLAGS -Wall -Werror -Wno-exit-time-destructors -Wno-unused-parameter)
target_compile_definitions(utils PRIVATE ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION)
target_link_libraries(utils PUBLIC cutils base log)

# --- libziparchive (incfs hardening off, as in the default library), libincfs-utils and libbuildversion.
sources_in(ziparchive_sources "${SOURCES}/libziparchive"
  zip_archive.cc zip_archive_stream_entry.cc zip_cd_entry_map.cc zip_error.cpp zip_writer.cc)
add_library(ziparchive STATIC ${ziparchive_sources})
# tools/aapt2/gtest holds gtest_prod.h, which zip_writer.h includes for FRIEND_TEST.
target_include_directories(ziparchive PUBLIC "${SOURCES}/libziparchive/include" "${CMAKE_CURRENT_SOURCE_DIR}"
                           PRIVATE "${SOURCES}/libziparchive/incfs_support/include")
soong_module(ziparchive FLAGS -Werror)
target_compile_definitions(ziparchive PRIVATE ZLIB_CONST INCFS_SUPPORT_DISABLED=1)
target_link_libraries(ziparchive PUBLIC base log ZLIB::ZLIB)

add_library(incfs_utils STATIC "${SOURCES}/incremental_delivery/incfs/util/map_ptr.cpp")
target_include_directories(incfs_utils PUBLIC "${SOURCES}/incremental_delivery/incfs/util/include")
target_link_libraries(incfs_utils PUBLIC base utils)
soong_module(incfs_utils)

add_library(buildversion STATIC "${SOURCES}/build_soong/cc/libbuildversion/libbuildversion.cpp")
target_include_directories(buildversion PUBLIC "${SOURCES}/build_soong/cc/libbuildversion/include")
soong_module(buildversion)

# --- libandroidfw for tools, with libandroidfw_pathutils.
set(androidfw "${FRAMEWORKS}/libs/androidfw")
sources_in(androidfw_sources "${androidfw}"
  ApkAssets.cpp ApkParsing.cpp Asset.cpp AssetDir.cpp AssetManager.cpp AssetManager2.cpp AssetsProvider.cpp
  AttributeResolution.cpp BigBuffer.cpp BigBufferStream.cpp ChunkIterator.cpp ConfigDescription.cpp FileStream.cpp
  Idmap.cpp LoadedArsc.cpp Locale.cpp LocaleData.cpp misc.cpp NinePatch.cpp ObbFile.cpp PosixUtils.cpp Png.cpp
  PngChunkFilter.cpp PngCrunch.cpp ResourceTimer.cpp ResourceTypes.cpp ResourceUtils.cpp StreamingZipInflater.cpp
  StringPool.cpp TypeWrappers.cpp Util.cpp ZipFileRO.cpp ZipUtils.cpp PathUtils.cpp)
add_library(androidfw STATIC ${androidfw_sources})
target_include_directories(androidfw PUBLIC "${androidfw}/include" "${androidfw}/include_pathutils" "${ndk_headers}")
# libandroidfw_defaults: cpp_std gnu++2b.
set_target_properties(androidfw PROPERTIES CXX_STANDARD 23)
soong_module(androidfw FLAGS -Werror -Wunreachable-code)
target_compile_definitions(androidfw PUBLIC STATIC_ANDROIDFW_FOR_TOOLS)
target_link_libraries(androidfw PUBLIC utils cutils ziparchive base log incfs_utils png ZLIB::ZLIB)

# --- aapt2: its .proto files through our protoc, then libaapt2 and the binary. The protos import each other as
# frameworks/base/tools/aapt2/…, so protoc reads them from a tree with that path.
set(aapt2 "${FRAMEWORKS}/tools/aapt2")
set(proto_root "${CMAKE_CURRENT_BINARY_DIR}/proto-root")
set(proto_out "${CMAKE_CURRENT_BINARY_DIR}/proto")
file(MAKE_DIRECTORY "${proto_root}/frameworks/base/tools" "${proto_out}")
file(CREATE_LINK "${aapt2}" "${proto_root}/frameworks/base/tools/aapt2" SYMBOLIC)
set(proto_names ApkInfo Configuration Resources ResourceMetadata ResourcesInternal)
set(proto_files "")
set(proto_inputs "")
set(proto_sources "")
foreach(name ${proto_names})
  list(APPEND proto_files "frameworks/base/tools/aapt2/${name}.proto")
  list(APPEND proto_inputs "${aapt2}/${name}.proto")
  list(APPEND proto_sources "${proto_out}/frameworks/base/tools/aapt2/${name}.pb.cc")
endforeach()
add_custom_command(
  OUTPUT ${proto_sources}
  COMMAND protoc --cpp_out=${proto_out} -I ${proto_root} ${proto_files}
  DEPENDS protoc ${proto_inputs}
  WORKING_DIRECTORY "${proto_root}"
  COMMENT "aapt2's protos")

sources_in(libaapt2_sources "${aapt2}"
  compile/IdAssigner.cpp compile/InlineXmlFormatParser.cpp compile/PseudolocaleGenerator.cpp compile/Pseudolocalizer.cpp
  compile/XmlIdCollector.cpp configuration/ConfigurationParser.cpp dump/DumpManifest.cpp filter/AbiFilter.cpp
  filter/ConfigFilter.cpp format/Archive.cpp format/Container.cpp format/binary/BinaryResourceParser.cpp
  format/binary/ResChunkPullParser.cpp format/binary/ResEntryWriter.cpp format/binary/TableFlattener.cpp
  format/binary/XmlFlattener.cpp format/proto/ProtoDeserialize.cpp format/proto/ProtoSerialize.cpp io/File.cpp
  io/FileSystem.cpp io/StringStream.cpp io/Util.cpp io/ZipArchive.cpp link/AutoVersioner.cpp
  link/FeatureFlagsFilter.cpp link/ManifestFixer.cpp link/NoDefaultResourceRemover.cpp link/PrivateAttributeMover.cpp
  link/ReferenceLinker.cpp link/ResourceExcluder.cpp link/TableMerger.cpp link/XmlCompatVersioner.cpp
  link/XmlNamespaceRemover.cpp link/XmlReferenceLinker.cpp optimize/MultiApkGenerator.cpp optimize/ResourceDeduper.cpp
  optimize/ResourceFilter.cpp optimize/Obfuscator.cpp optimize/VersionCollapser.cpp process/ProductFilter.cpp
  process/SymbolTable.cpp split/TableSplitter.cpp text/Printer.cpp text/Unicode.cpp text/Utf8Iterator.cpp
  util/Files.cpp util/Util.cpp Debug.cpp DominatorTree.cpp java/AnnotationProcessor.cpp java/ClassDefinition.cpp
  java/JavaClassGenerator.cpp java/ManifestClassGenerator.cpp java/ProguardRules.cpp LoadedApk.cpp Resource.cpp
  ResourceParser.cpp ResourceTable.cpp ResourceUtils.cpp ResourceValues.cpp SdkConstants.cpp trace/TraceBuffer.cpp
  xml/XmlActionExecutor.cpp xml/XmlDom.cpp xml/XmlPullParser.cpp xml/XmlUtil.cpp ValueTransformer.cpp
  Main.cpp cmd/ApkInfo.cpp cmd/Command.cpp cmd/Compile.cpp cmd/Convert.cpp cmd/Diff.cpp cmd/Dump.cpp cmd/Link.cpp
  cmd/Optimize.cpp cmd/Util.cpp)
# util/Files.h and .cpp declare BuildPath, which only aapt2's tests call, with std::vector<const StringPiece>: the
# Mac's libc++ refuses an allocator of const elements, which AOSP's libc++ at the tag still took. aapt2 compiles
# copies with std::vector<StringPiece> instead, made here from the sources; the patched header comes first on the
# include path. A change in those lines stops the configure step.
set(patched "${CMAKE_CURRENT_BINARY_DIR}/patched")
foreach(file util/Files.h util/Files.cpp)
  file(READ "${aapt2}/${file}" text)
  string(REGEX REPLACE "std::vector<const ((android::)?StringPiece)>" "std::vector<\\1>" patched_text "${text}")
  if(patched_text STREQUAL text)
    message(FATAL_ERROR "${aapt2}/${file} no longer has std::vector<const StringPiece>: check the patch")
  endif()
  file(WRITE "${patched}/${file}" "${patched_text}")
endforeach()
list(REMOVE_ITEM libaapt2_sources "${aapt2}/util/Files.cpp")
list(APPEND libaapt2_sources "${patched}/util/Files.cpp")

add_executable(aapt2 ${libaapt2_sources} ${proto_sources})
target_include_directories(aapt2 BEFORE PRIVATE "${patched}")
target_include_directories(aapt2 PRIVATE "${aapt2}" "${proto_out}" "${proto_out}/frameworks/base/tools/aapt2"
  "${SOURCES}/libnativehelper/include_jni" "${FRAMEWORKS}/cmds/idmap2/libidmap2_policies/include")
# aapt2_defaults: cpp_std gnu++2b.
set_target_properties(aapt2 PROPERTIES CXX_STANDARD 23)
soong_module(aapt2 FLAGS -Wall -Werror -Wno-unused-parameter
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers -fno-exceptions -fno-rtti -Wno-deprecated-declarations>)
target_compile_definitions(aapt2 PRIVATE _DARWIN_UNLIMITED_STREAMS)
# The rest comes through libandroidfw.
target_link_libraries(aapt2 PRIVATE androidfw expat protobuf buildversion)
