# Third-party code

What Minato adopts from others, with license and version. Project license: Apache-2.0 (ADR-0017). GPL and AGPL code is not adopted, with one exception (ADR-0033): ART's code under GPL-2.0 with the Classpath Exception, which ships as separate components with its license text and its source code (below).

The app carries the license texts of what it embeds in `Licenses/`: `tools/bundle-licenses.py` copies them at build time as [`third_party/licenses.json`](third_party/licenses.json) lists them, and `tools/license-check.py` fails a built app that lacks a text for a table row of an embedded section below.

## Guest libc (ADR-0009, ADR-0021)

AOSP sources at the tag `android-15.0.0_r1`, from `android.googlesource.com`. `tools/fetch-sources.py` downloads them to `build/sources/` (not in the repo) and checks them against the checksums in [`third_party/sources.json`](third_party/sources.json). From them, `tools/build-bionic.sh` builds `libc.so`, `libm.so`, `libdl.so` and `ld-android.so` with the NDK. These libraries run as guest code in the process.

| Source | Commit | License | Which parts |
|---|---|---|---|
| `platform/bionic` | `361ba86734fb` | BSD-style and Apache-2.0, per file (`libc/NOTICE`) | libc, libm, libdl; build descriptions, generators (`gensyscalls.py`, `generate-version-script.py`); from linker64 `linker/arch/arm64/tlsdesc_resolver.S` (BSD), built into our `libc.so` |
| `platform/external/scudo` | `67dd481e24c7` | Apache-2.0 WITH LLVM-exception | Allocator of the libc |
| `platform/external/gwp_asan` | `df96c8131b0e` | Apache-2.0 WITH LLVM-exception | required by the libc |
| `platform/external/arm-optimized-routines` | `514df029da8a` | MIT OR Apache-2.0 WITH LLVM-exception | `string/` and `math/`: string and math routines for arm64 |
| `platform/system/core` | `fc7bc8c4bfb4` | Apache-2.0 | 8 files: libcutils headers, property parser |
| `platform/build` | `3d9c9aacdb6c` | Apache-2.0 | `fs_config_generator.py`: generates the table of Android UIDs |

**Our changes to Bionic** (`runtime/bionic/`, ADR-0021):
- `overlay/bionic/libc/arch-arm64/bionic/setjmp.S`: Bionic's file, without saving and restoring `x18` (ADR-0019). License as in the original (BSD, header in the file).
- `scudo/custom_scudo_config.h`: Scudo's Android configuration (`config/custom_scudo_config.h` of `platform/external/scudo` at `67dd481e24c7`), with regions of 64 MiB, each reserved when its size class is first used, instead of 256 MiB in one piece (`RegionSizeLog` 26, `EnableContiguousRegions` false; ADR-0035). License as in the original: AOSP's BSD header and Apache-2.0 WITH LLVM-exception, both in the file.
- `src/libc_start.cpp`, `src/libc_start.map`, `src/sigreturn.S`, `src/ld_android.cpp`, `src/ld_android.map`: our own code (Apache-2.0), built with Bionic's private headers.
- `src/liblog.cpp`, `src/libnetd_client.cpp`: our own small versions of `liblog.so` and `libnetd_client.so` (Apache-2.0), based on the NDK documentation and on what Bionic expects of the library, respectively; no AOSP code adopted.

**Derived from Bionic in the runtime** (host code, ADR-0039 a):
- `runtime/src/properties.cpp`: the system property area that the runtime writes for `/dev/__properties__`, in Bionic's pre-split format. It follows `libc/system_properties/prop_area.cpp` (`prop_area::add` and the trie functions) and the layouts of `prop_area.h` and `prop_info.h` at `361ba86734fb`. License as in the original: AOSP's BSD header, kept in the file.

**From the Android NDK r30** (`30.0.16248370`) in the guest libraries:

| Part | License | What for |
|---|---|---|
| LLVM libunwind (`libunwind.a`) | Apache-2.0 WITH LLVM-exception | in `libc.so`, like Android's `libunwind-exported`; `tools/export-archive-symbols.py` makes its symbols exportable |
| compiler-rt builtins | Apache-2.0 WITH LLVM-exception | Runtime helpers of the compiler (e.g. 128-bit floating point) |
| `crtbegin_so.o`, `crtend_so.o` | BSD (Bionic) | Start and end of `libm.so`, `libdl.so`, `ld-android.so` |
| `android/log.h` | Apache-2.0 | Header for the libc (liblog) |
| zlib (`libz.a`) | Zlib | `libz.so` (ADR-0028): zlib `1.3.2.1-motley` as AOSP ships it, linked whole |

## Graphics: ANGLE (ADR-0029)

ANGLE's branch `chromium/8037` at `b4cbb0a8`, from `chromium.googlesource.com/angle/angle`. `tools/build-angle.sh` fetches it with the dependencies it keeps into `build/angle/` (not in the repo), pinned in [`third_party/sources.json`](third_party/sources.json), and builds `libGLESv2` with the Metal backend only. The app embeds it as `libGLESv2.framework` from `build/ANGLE.xcframework`. Our changes are the patch series in `third_party/angle/patches/`, each with a header that says why, whether it could go upstream and when it goes.

| Part in `libGLESv2` | Version | License |
|---|---|---|
| ANGLE | `chromium/8037` @`b4cbb0a8` | BSD-3-Clause |
| Chromium's base helpers (`src/common/base`) | with ANGLE | BSD-3-Clause |
| xxHash (`src/common/third_party/xxhash`) | with ANGLE | BSD-2-Clause |
| Abseil (`third_party/abseil-cpp`) | `435e7d97` | Apache-2.0 |
| Chromium's libc++, libc++abi and llvm-libc, linked in and hidden | `97b436da`, `fc1897a2`, `32082418` | Apache-2.0 WITH LLVM-exception |
| zlib (`third_party/zlib`) | `5eb4d7ed` | Zlib |

The BSD licenses ask for their notices with every binary: the app embeds the framework since 2026-09-27 and carries ANGLE's texts in `Licenses/`, with the holders of `src/common/base` (The Chromium Authors, the V8 project authors) from [`third_party/angle/licenses/`](third_party/angle/licenses/). Used only to build, not in the app: depot_tools' `gclient.py` (BSD-3-Clause), GN (BSD-3-Clause), Chromium's clang (Apache-2.0 WITH LLVM-exception), and the Khronos registries `gl.xml` and `egl.xml` (Apache-2.0).

## Graphics: MoltenVK (ADR-0030)

MoltenVK 1.4.2 as Khronos released it (tag `v1.4.2`, commit `db660224`), from `github.com/KhronosGroup/MoltenVK/releases`. `tools/fetch-moltenvk.sh` fetches it into `build/moltenvk/` (not in the repo), checked against the SHA-256 in [`third_party/sources.json`](third_party/sources.json). The app embeds the dynamic framework for the device from `build/MoltenVK.xcframework`; nothing of it is built or changed.

| Part in `MoltenVK.framework` | Version | License |
|---|---|---|
| MoltenVK | `v1.4.2` @`db660224` | Apache-2.0 |
| SPIRV-Cross | `6c09849f` | Apache-2.0 |
| SPIRV-Tools | `0d6fd73c` | Apache-2.0 |
| cereal (headers only) | `a56bad8b` | BSD-3-Clause |
| SPIRV-Headers (headers only) | `29981f65` | MIT |
| Vulkan-Headers (headers only) | `v1.4.357` @`e3b1eec0` | Apache-2.0 OR MIT |

Only MoltenVK's own license text comes with the release. The texts of SPIRV-Cross, SPIRV-Tools, SPIRV-Headers and cereal, at these commits, are in [`third_party/moltenvk/licenses/`](third_party/moltenvk/licenses/) (`tools/fetch-moltenvk.sh --licenses`, checked against their Git blobs), for the bundle. Used only to build: `vk.xml` of Vulkan-Headers `v1.4.357` (Apache-2.0 OR MIT), read by the generator of ADR-0030, and the Vulkan headers of the same version from MoltenVK's release, against which our guest `libvulkan.so` is built.

## Parity benchmark (ADR-0037)

Only in benchmark builds (`APK_EMBED_BENCHMARK=YES`), beside `MinatoBench.framework`: the APKs `bench-gles` and `bench-vulkan` in `Benchmark/`, built by `tools/build-bench.sh` with the NDK r30 (`30.0.16248370`). `MinatoBench.framework` is our own code and links ANGLE's framework (above); neither APK links libc++.

| Part | License | What for |
|---|---|---|
| `native_app_glue` | Apache-2.0 | `android_native_app_glue.c`, linked into both APKs' libraries |

Used only to build: the NDK's `glslc` (Apache-2.0), which compiles the scene's shaders for both builds.

## Time zones

Bionic reads time zones from a file in its own format. At build time, `tools/make-tzdata.py` packs them from the Mac's IANA time zone database (`/usr/share/zoneinfo`, public domain) into `build/bionic/system/usr/share/zoneinfo/tzdata`; not in the repo.

## ART and its libraries (ADR-0033)

AOSP sources at the tag `android-15.0.0_r1`, from `android.googlesource.com`. `tools/fetch-sources.py --set art` downloads them to `build/sources/` (not in the repo) and checks them against the checksums in the list `art` of [`third_party/sources.json`](third_party/sources.json). Step a) builds guest code from them with the NDK; from step d) on, the app embeds what that build makes, and until then nothing of them is in the app.

| Source | Commit | License | Which parts |
|---|---|---|---|
| `platform/art` | `bebbc3cc49f2` | Apache-2.0 (`NOTICE`); `openjdkjvm/` GPL-2.0 with Classpath Exception (`openjdkjvm/LICENSE`), as are `openjdkjvmti/` and `dt_fd_forward/`, which are not built | the runtime (`libart`), `dalvikvm`, `libopenjdkjvm` |
| `platform/libcore` | `a996d969fdd1` | Apache-2.0 with the notices in `NOTICE` (Android, Apache Harmony, ICU, KXML, W3C); `ojluni/` GPL-2.0 with Classpath Exception (`LICENSE`) | the boot jars, among them core-oj from `ojluni/`; their natives `libjavacore` and `libopenjdk` |
| `platform/external/icu` | `cf305aeb6df4` (ICU 75.1) | Unicode-3.0 and the notices in `LICENSE`; the GPL texts there cover ICU4C's Autoconf files, which are not used | ICU's libraries and data, `libicu_jni` |
| `platform/external/boringssl` | `23a87e389eb9` | OpenSSL, SSLeay, ISC and others, as in `LICENSE` | `libcrypto` of ART's module; `libcrypto_static` and libssl's static variant for Conscrypt's `libjavacrypto` (ADR-0046), tested by `boringssl-test` |
| `platform/external/conscrypt` | `f8e2d8143241` | Apache-2.0 (`LICENSE`, `MODULE_LICENSE_APACHE2`), with portions of Netty and Apache Harmony, also Apache-2.0 (`NOTICE`) | `conscrypt.jar` and `libjavacrypto` (ADR-0046 c), built into `build/art`; not in the app until ADR-0033 d) |
| `platform/external/bouncycastle` | `9be82ede5e65` | MIT, the Bouncy Castle License (`NOTICE`, `MODULE_LICENSE_BSD_LIKE`); six files AOSP added are Apache-2.0 (`crypto/digests`' `AndroidDigestFactory*` and `OpenSSLDigest`, `jce/provider/CertBlocklist`) | `bouncycastle.jar` from `repackaged/bcprov`, shrunk by R8 (ADR-0046 d), built into `build/art`; not in the app until ADR-0033 d) |
| `platform/external/okhttp` | `fdad2b22b007` | Apache-2.0 (`LICENSE.txt`, `MODULE_LICENSE_APACHE2`), okio's repackaged tree included | `okhttp.jar` from the five repackaged trees (ADR-0046 d, which took in step e), built into `build/art`; not in the app until ADR-0033 d) |
| `platform/system/ca-certificates` | `2207c6dd74f1` | no license text: AOSP's `METADATA` says `license_type: BY_EXCEPTION_ONLY` ("this package has special licensing conditions") and notes that QuoVadis's certificate policy in `files/5cf9d536.0` references the QuoVadis Certificate Holder Agreement | fetched for ADR-0046: Android 15's 145 root certificates for step f), not in the app yet |
| `platform/libnativehelper` | `3ca43dfe2bf4` | Apache-2.0 | `libnativehelper` (JNI helpers) |
| `platform/external/fdlibm` | `1e651e1ef2b6` | Sun Microsystems' notice (`NOTICE`) | math for libcore's `StrictMath` |
| `platform/external/expat` | `8ae3fff00472` | MIT (`COPYING`) | XML parser for libcore |
| `platform/external/zlib` | `86056a43326c` | Zlib | only its headers: ART's libraries link the guest system's `libz.so`, the NDK's zlib |
| `platform/external/lz4` | `1d69e7802438` | BSD-2-Clause for `lib/`, the only part used; the rest of the repository GPL-2.0, not used | LZ4 for ART |
| `platform/external/lzma` | `b744809d5506` | public domain | xz for ART |
| `platform/external/libcap` | `c2c4eed91fe3` | BSD-3-Clause OR GPL-2.0-only, used under BSD-3-Clause | for ART |
| `platform/external/tinyxml2` | `d3f5ba7b82e0` | Zlib | XML for ART |
| `platform/external/cpu_features` | `eca53ba6d2e9` | Apache-2.0 | CPU features for ART |
| `platform/external/fmtlib` | `360e74bb8ec7` | MIT | formatting, which `libbase` links |
| `platform/system/libbase` | `68f963c62f7f` | Apache-2.0 | `libbase` |
| `platform/system/libziparchive` | `13b815f48578` | Apache-2.0 | `libziparchive` (APKs and jars) |
| `platform/system/libprocinfo` | `e2d8c69e0454` | Apache-2.0 | `libprocinfo` |
| `platform/system/unwinding` | `63e40770259e` | Apache-2.0 | `libunwindstack`, for ART's stack dumps |
| `platform/external/dlmalloc` | `f18b9ade29f2` (dlmalloc 2.8.6) | public domain (CC0-1.0, `NOTICE`) | the allocator of ART's dlmalloc spaces, compiled into `libart` |
| `platform/system/logging` | `54d3fa266d21` | Apache-2.0 (`liblog/NOTICE`) | only `liblog/include`: the headers ART and libcore compile against; `liblog.so` is Minato's own |
| `platform/system/timezone` | `fba551ac75e9` | Apache-2.0 | only read: the format of the time zone files (ADR-0033, decision 9); nothing of it ships |

**GPL-2.0 with the Classpath Exception** (ADR-0033): core-oj from libcore's `ojluni/`, `libopenjdk` and `libopenjdkjvm` ship as separate components, each with its license text and its source code. ADR-0033 also names libcore's OpenSSL parts: at this commit, OpenSSL's text in libcore is only in a test resource (`support/src/test/java/tests/resources/x509/default.cnf`), which is not built; the OpenSSL license that ships is BoringSSL's. Step a)'s build (`tools/art-plan.py`) puts code under this license only into core-oj.jar (`ojluni/`'s Java), `libopenjdk` and `libopenjdkd` (its natives), and `libopenjdkjvm` and `libopenjdkjvmd` (`openjdkjvm/`).

## Build tools for ART (ADR-0033)

Used only to build ART's Java part, not in the app. `tools/fetch-sources.py --set art` fetches them with the sources above, the JDK checked against Adoptium's SHA-256 before it is unpacked.

| Tool | Version | License |
|---|---|---|
| Eclipse Temurin JDK, macOS aarch64, in `build/tools/jdk-17/` | `17.0.20.1+1` (SHA-256 `196d13ba5f10`) | GPL-2.0 with Classpath Exception |
| D8 and R8 (`platform/prebuilts/r8`, one jar), the one the boot jars of the tag were built with; R8 shrinks `bouncycastle.jar` (ADR-0046 d) | `535c14aebec5` | BSD-3-Clause |
| Soong's R8 rules (`platform/build`'s `core/proguard.flags`, which includes `core/proguard_basic_keeps.flags` and `core/proguard/kotlin.flags`), with which R8 shrinks `bouncycastle.jar` as Soong does (ADR-0046 d) | `3d9c9aacdb6c` | Apache-2.0 |

## Build tools for the framework (ADR-0045)

Host tools in `build/`, not in the app. The Java that `aidl` writes comes from AOSP's own AIDL files.

| Tool | Version | License |
|---|---|---|
| `aidl` (`platform/system/tools/aidl`), built for the Mac by `tools/build-aidl.sh` into `build/aidl/`, fetched by `tools/fetch-sources.py --set framework` | `d1304733b74b` (android-15.0.0_r1) | Apache-2.0 |
| GNU Bison, from Homebrew (keg-only, `/opt/homebrew/opt/bison`), which generates `aidl`'s parser | 3.8.2 | GPL-3.0-or-later; the generated parser falls under Bison's skeleton exception |
| fmtlib's `src/format.cc`, linked into the host `aidl` as Soong links its static fmtlib | the ART row's commit | MIT |

## Framework sources (ADR-0039 step b)

Fetched by `tools/fetch-sources.py --set framework` at android-15.0.0_r1 (42.7 MB packed, your go). The host aapt2 and protoc, with the protobuf, libpng, expat, libutils, libcutils, libziparchive, incfs-utils and libbuildversion they link, are build tools in `build/` and do not ship; what the guest system ships is named here when its step builds it.

| Project | Commit | License | What we take |
|---|---|---|---|
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | only `libs/androidfw`, `core/res`, `core/jni`, `core/java/android`, `core/java/com/android/internal` and `opengl/java`: libandroidfw, framework-res's sources, the framework's natives and Java, and aapt2 |
| `platform/external/protobuf` | `aeea14007d1a` | BSD-3-Clause | only `src/`: libprotobuf and protoc for the host aapt2 |
| `platform/external/libpng` | `6a336e4372cf` | libpng License v2 | aapt2's PNG crunching and libandroidfw |
| `platform/system/core` | `fc7bc8c4bfb4` | Apache-2.0 | only `libutils`, `libcutils` and `libsystem`, which aapt2 and libandroidfw link |
| `platform/system/incremental_delivery` | `fcd1a7622ab4` | Apache-2.0 | libincfs-utils, which libandroidfw links |
| `platform/build/soong` | `3fe78dc9b32f` | Apache-2.0 | only `cc/libbuildversion`, which aapt2 links |
| `platform/build/release` | `e1dfb3dc7831` | Apache-2.0 | only `aconfig/ap3a`: Android 15's flag values, read by our flag generator |
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `graphics/java` and `ravenwood/annotations-src` (3.0 MB): the graphics and annotation classes that framework.jar's taken files name, as compile-only stubs or taken files (ADR-0039 b3) |
| `platform/frameworks/libs/modules-utils` | `810bd8c57bb3` | Apache-2.0 | only `java/` (0.5 MB): `Preconditions`, the typed XML parsers and annotations that framework.jar's taken files use (ADR-0039 b3) |

b3.2 builds `libandroidfw` for the guest, with `libbase`, fmtlib, `libcutils`, `libutils`, `libziparchive`, `libincfs-utils` and libpng, as static archives; they ship inside `libandroid_runtime.so` from b3.3 on. `libandroidfw` carries Minato's patch `framework/native/patches/androidfw-directory-listing.patch` (Apache-2.0 like the file it changes): it only changes `DirectoryAssetsProvider::ForEachFile` in `libs/androidfw/AssetsProvider.cpp` and adds three includes there.

b3.3 builds `libandroid_runtime.so` from `core/jni`'s files for Log, AssetManager, StringBlock, XmlBlock, ApkAssets and SystemProperties, unchanged, with stand-ins of ours (`framework/native/android_runtime`). framework.jar carries Minato's patch `framework/patches/assetmanager-framework-res-folder.patch` to `core/java/android/content/res/AssetManager.java` (Apache-2.0 like the file it changes): only `createSystemAssetsInZygoteLocked` changes, to open framework-res's folder and ask for no overlay idmaps.

The API signature files of the managers' generator (ADR-0045 decision 3), fetched the same way at the tag on your go in the App chat. `tools/managergen` reads them and writes our own classes under their names into `build/`; the files themselves never ship.

| Project | Commit | License | File | Size | SHA-256 |
|---|---|---|---|---|---|
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `core/api/current.txt` | 4,382,010 | `a07b0a1df1b7aeeb…` |
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `location/api/current.txt` | 53,006 | `a0495a8a32384610…` |
| `platform/packages/modules/Connectivity` | `355670c19612` | Apache-2.0 | `framework/api/current.txt` | 55,123 | `26bf1910035039b2…` |
| `platform/packages/modules/Connectivity` | `355670c19612` | Apache-2.0 | `framework-t/api/current.txt` | 20,275 | `2a4b39ab959d50a3…` |
| `platform/packages/modules/Wifi` | `a88d7fbb026b` | Apache-2.0 | `framework/api/current.txt` | 134,668 | `c10ce9a2f78634c2…` |

The platform's split permissions (ADR-0045 decision 6, ADR-0039 b3.5b1), fetched the same way on your go in the App chat. The guest reads the file unchanged at `/system/etc/permissions/platform.xml`, as Android's SystemConfig does. Today only the Mac's harness mounts it; it ships in the guest system from step c on.

| Project | Commit | License | File | Size | SHA-256 |
|---|---|---|---|---|---|
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `data/etc/platform.xml` | 18,104 | `4f1139ad764d74f3…` |

## Graphics sources (ADR-0043 a)

Fetched by `tools/fetch-sources.py --set graphics` at android-15.0.0_r1 (22.6 MB packed, your go; the headers of frameworks/native's `include/android` and `opengl/include` and libhardware's `hardware.h` 0.25 MB more, your go of 2026-10-02). Nothing of it ships before ADR-0043 a1 builds it.

| Project | Commit | License | What we take |
|---|---|---|---|
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `libs/hwui` (the host flavor and its graphics JNI), `libs/hostgraphics`; `data/fonts` is under "Guest fonts" below |
| `platform/external/skia` | `0fe593a9a878` | BSD-3-Clause (`LICENSE`) | the CPU backend: `include`, `src` without the GPU sources, `modules/skcms`, `client_utils/android` |
| `platform/frameworks/minikin` | `b41e852cee7f` | Apache-2.0 | text layout, with its C++ hyphenator (the Rust one is left out) |
| `platform/external/harfbuzz_ng` | `9a50c421c9ca` | Old MIT (`COPYING`); the GPL texts in `LICENSE` cover files that are not compiled | text shaping (`src`), and the subsetter Skia's PDF uses |
| `platform/external/freetype` | `5ab8c8be9c2c` | FreeType License (`docs/FTL.TXT`), taken of the FTL or GPL-2.0 it offers; the FTL asks for a credit in our documentation | font rasterizer (`src`, `include`) |
| `platform/external/libjpeg-turbo` | `1e113e5593fa` | IJG, BSD-3-Clause and zlib (`LICENSE.md`) | JPEG codec |
| `platform/external/webp` | `c3a8e58734bd` | BSD-3-Clause (`COPYING`), with Google's patent grant (`PATENTS`) | WebP codec (`src`, `sharpyuv`) |
| `platform/external/wuffs-mirror-release-c` | `479408ccc13b` | Apache-2.0 | GIF decoder |
| `platform/external/libultrahdr` | `448f96ed8315` | Apache-2.0 | JPEG gain maps |
| `platform/external/image_io` | `37e2347000d7` | Apache-2.0 | XMP and container parsing for libultrahdr |
| `platform/external/modp_b64` | `d88d4daed951` | BSD-style (`LICENSE`) | base64 for image_io |
| `platform/frameworks/native` | `f7274fca5e36` | Apache-2.0; in `opengl/include` the Khronos headers MIT (`MODULE_LICENSE_MIT`), ANGLE's `EGL/eglext_angle.h` and `EGL/Platform.h` BSD-3-Clause (as ANGLE above), `GLES3/gl3ext.h` SGI Free Software License B 2.0 | headers (`include/private`, `include/android`, `opengl/include`, libgui's `TraceUtils.h`, nativebase, nativedisplay, nativewindow, binder NDK, math), `libs/ui`'s types, `libs/arect` |
| `platform/external/vulkan-headers` | `d3602c8e1b0b` | Apache-2.0 or MIT (`LICENSE.md`) | the 12 C headers hwui includes |
| `platform/frameworks/av` | `1863cb0ea5a9` | Apache-2.0 | `media/ndk/include` |
| `platform/hardware/interfaces` | `488942f82bd1` | Apache-2.0 (`NOTICE`) | the AIDL of graphics common and composer3, for their enum headers |
| `platform/hardware/libhardware` | `58faa60aadb6` | Apache-2.0 (`NOTICE`) | `hardware.h`, which libui's `PixelFormat.h` includes |
| `platform/system/server_configurable_flags` | `3b6d3e8d7d20` | Apache-2.0 | `libflags` |
| `platform/external/googletest` | `c05eedac6f5e` | BSD-3-Clause | `gtest_prod.h`, which minikin includes |

## Guest fonts (ADR-0043 test 2)

From the graphics set above. The guest's `/system/fonts` holds these 4 fonts (6.8 MB), and its `/system/etc` our cuts of AOSP's two font lists, `graphics/fonts/fonts.xml` and `font_fallback.xml` (Apache-2.0, as the originals; each names its source). `tools/build-bionic.sh` puts them into `build/bionic/system` (`graphics/fonts/fonts.cmake`), and the app carries them in `Guest/system` (`tools/embed-guest-system.sh`).

| Project | Commit | License | What we take |
|---|---|---|---|
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 (`data/fonts/NOTICE`) | from `data/fonts`: `DroidSansMono.ttf`, `DroidSansFallbackFull.ttf`, and `fonts.xml` and `font_fallback.xml`, which ours are cut from |
| `platform/external/roboto-fonts` | `4029d6e45cb3` | Apache-2.0 (`NOTICE`) | `Roboto-Regular.ttf`, `RobotoStatic-Regular.ttf` |

## References without adopted code

- **ARTBox** (MIT, `fd48d48`): The list of Bionic sources and their commits for `android-15.0.0_r1` served as the starting point; the commits have been checked against the tag on `android.googlesource.com`. No code is adopted.
- **Certificate from SideStore and AltStore** (ADR-0023, research in [`docs/research/store-certificates.md`](docs/research/store-certificates.md)). Only read:
  - which URL the export deep link has and which placeholders it replaces;
  - which markers the stores write into the Info.plist;
  - how SideStore builds its `.p12`.

  No code is adopted. The PKCS#12 reader and the callback parser are our own code, written from RFC 7292 and from the measured behavior.

  | Project | Version | License |
  |---|---|---|
  | LiveContainer | `main` @`4dbe0f9a` (3.8.10) | AGPL-3.0 |
  | SideStore | `0.7.0-alpha` @`6032424`, `develop` @`0dd743f` | AGPL-3.0 |
  | AltStore Classic | `classic` @`fafd76ee` (2.3) | AGPL-3.0 |
  | AltSign | `0d3c1a3c` | no license file |
  | SideSign | `6b68651` | no license file |
  | CodeSignKit | `d0c6771` | not stated |
