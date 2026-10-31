# Third-party code

What Minato adopts from others, with license and version. Project license: Apache-2.0 (ADR-0017). GPL and AGPL code is
not adopted, with one exception (ADR-0033): ART's code under GPL-2.0 with the Classpath Exception, which ships as
separate components with its license text, and whose source code must go with every build that leaves the Mac (below).

The app carries the license texts of what it embeds in `Licenses/`: `tools/bundle-licenses.py` copies them at build time
as [`third_party/licenses.json`](third_party/licenses.json) lists them, and `tools/license-check.py` fails a built app
that lacks a text for a table row of an embedded section below.

## Guest libc (ADR-0009, ADR-0021)

AOSP sources at the tag `android-15.0.0_r1`, from `android.googlesource.com`. `tools/fetch-sources.py` downloads them to
`build/sources/` (not in the repo) and checks them against the checksums in
[`third_party/sources.json`](third_party/sources.json). From them, `tools/build-bionic.sh` builds `libc.so`, `libm.so`,
`libdl.so` and `ld-android.so` with the NDK. These libraries run as guest code in the process.

| Source | Commit | License | Which parts |
|---|---|---|---|
| `platform/bionic` | `361ba86734fb` | BSD-style and Apache-2.0, per file (`libc/NOTICE`) | libc, libm, libdl; build descriptions, generators (`gensyscalls.py`, `generate-version-script.py`); from linker64 `linker/arch/arm64/tlsdesc_resolver.S` (BSD), built into our `libc.so` |
| `platform/external/scudo` | `67dd481e24c7` | Apache-2.0 WITH LLVM-exception | Allocator of the libc |
| `platform/external/gwp_asan` | `df96c8131b0e` | Apache-2.0 WITH LLVM-exception | required by the libc |
| `platform/external/arm-optimized-routines` | `514df029da8a` | MIT OR Apache-2.0 WITH LLVM-exception | `string/` and `math/`: string and math routines for arm64 |
| `platform/system/core` | `fc7bc8c4bfb4` | Apache-2.0 | 8 files: libcutils headers, property parser |
| `platform/build` | `3d9c9aacdb6c` | Apache-2.0 | `fs_config_generator.py`: generates the table of Android UIDs |

**Our changes to Bionic** (`runtime/bionic/`, ADR-0021):
- `overlay/bionic/libc/arch-arm64/bionic/setjmp.S`: Bionic's file, without saving and restoring `x18` (ADR-0019).
  License as in the original (BSD, header in the file).
- `scudo/custom_scudo_config.h`: Scudo's Android configuration (`config/custom_scudo_config.h` of
  `platform/external/scudo` at `67dd481e24c7`), with regions of 64 MiB, each reserved when its size class is first used,
  instead of 256 MiB in one piece (`RegionSizeLog` 26, `EnableContiguousRegions` false; ADR-0035). License as in the
  original: AOSP's BSD header and Apache-2.0 WITH LLVM-exception, both in the file.
- `src/libc_start.cpp`, `src/libc_start.map`, `src/sigreturn.S`, `src/ld_android.cpp`, `src/ld_android.map`: our own
  code (Apache-2.0), built with Bionic's private headers.
- `src/liblog.cpp`, `src/libnetd_client.cpp`: our own small versions of `liblog.so` and `libnetd_client.so`
  (Apache-2.0), based on the NDK documentation and on what Bionic expects of the library, respectively; no AOSP code
  adopted.

**Derived from Bionic in the runtime** (host code, ADR-0039 a):
- `runtime/src/properties.cpp`: the system property area that the runtime writes for `/dev/__properties__`, in Bionic's
  pre-split format. It follows `libc/system_properties/prop_area.cpp` (`prop_area::add` and the trie functions) and the
  layouts of `prop_area.h` and `prop_info.h` at `361ba86734fb`. License as in the original: AOSP's BSD header, kept in
  the file.

**Derived from AOSP in libandroid** (`runtime/android/`, the guest's `lib64/libandroid.so`):
- `runtime/android/sharedmem.cpp`: the NDK's ASharedMemory, from frameworks/base's `native/android/sharedmem.cpp` and
  the memfd path of system/core's `libcutils/ashmem-dev.cpp` at android-15.0.0_r1, merged into one file in place of
  libcutils, which the guest system lacks. License as in the originals: AOSP's Apache-2.0 header, kept in the file.

**From the Android NDK r30** (`30.0.16248370`) in the guest libraries:

| Part | License | What for |
|---|---|---|
| LLVM libunwind (`libunwind.a`) | Apache-2.0 WITH LLVM-exception | in `libc.so`, like Android's `libunwind-exported`; `tools/export-archive-symbols.py` makes its symbols exportable |
| compiler-rt builtins | Apache-2.0 WITH LLVM-exception | Runtime helpers of the compiler (e.g. 128-bit floating point) |
| `crtbegin_so.o`, `crtend_so.o` | BSD (Bionic) | Start and end of `libm.so`, `libdl.so`, `ld-android.so` |
| `android/log.h` | Apache-2.0 | Header for the libc (liblog) |
| zlib (`libz.a`) | Zlib | `libz.so` (ADR-0028): zlib `1.3.2.1-motley` as AOSP ships it, linked whole |
| LLVM libc++ (`libc++_shared.so`) | Apache-2.0 WITH LLVM-exception | ART's C++ library, which `runtime/art/CMakeLists.txt` copies into `build/art/lib` (ADR-0033 decision 2); in `com.android.art`'s lib64 since ADR-0033 d2 |

## Graphics: ANGLE (ADR-0029)

ANGLE's branch `chromium/8037` at `b4cbb0a8`, from `chromium.googlesource.com/angle/angle`. `tools/build-angle.sh`
fetches it with the dependencies it keeps into `build/angle/` (not in the repo), pinned in
[`third_party/sources.json`](third_party/sources.json), and builds `libGLESv2` with the Metal backend. The iPhone's
slice and the Mac's test library also have the Vulkan backend (ADR-0044 c), the Simulator's slice not, with Volk from
ANGLE's tree and four more of the dependencies it keeps, in the table below. The app embeds it as `libGLESv2.framework`
from `build/ANGLE.xcframework`. Our changes are the patch series in `third_party/angle/patches/`, each with a header
that says why, whether it could go upstream and when it goes.

| Part in `libGLESv2` | Version | License |
|---|---|---|
| ANGLE | `chromium/8037` @`b4cbb0a8` | BSD-3-Clause |
| Chromium's base helpers (`src/common/base`) | with ANGLE | BSD-3-Clause |
| xxHash (`src/common/third_party/xxhash`) | with ANGLE | BSD-2-Clause |
| Abseil (`third_party/abseil-cpp`) | `435e7d97` | Apache-2.0 |
| Chromium's libc++, libc++abi and llvm-libc, linked in and hidden | `97b436da`, `fc1897a2`, `32082418` | Apache-2.0 WITH LLVM-exception |
| zlib (`third_party/zlib`) | `5eb4d7ed` | Zlib |
| Volk (`src/third_party/volk`) | with ANGLE | MIT |
| Vulkan-Headers (`third_party/vulkan-headers/src`), headers only | `31386378` | Apache-2.0 OR MIT, used under Apache-2.0 |
| SPIRV-Headers (`third_party/spirv-headers/src`), headers only | `49654312` | MIT |
| SPIRV-Tools (`third_party/spirv-tools/src`) | `b40380bf` | Apache-2.0 |
| VulkanMemoryAllocator (`third_party/vulkan_memory_allocator`) | `7e55b011` | MIT |

The BSD licenses ask for their notices with every binary: the app embeds the framework since 2026-09-27 and carries
ANGLE's texts in `Licenses/`, with the holders of `src/common/base` (The Chromium Authors, the V8 project authors) from
[`third_party/angle/licenses/`](third_party/angle/licenses/). Used only to build, not in the app: depot_tools'
`gclient.py` (BSD-3-Clause), GN (BSD-3-Clause), Chromium's clang (Apache-2.0 WITH LLVM-exception), and the Khronos
registries `gl.xml` and `egl.xml` (Apache-2.0).

## Graphics: MoltenVK (ADR-0030, ADR-0044 b))

MoltenVK 1.4.2 (tag `v1.4.2`, commit `db660224`), built by us from Khronos's sources since ADR-0044 b):
`tools/fetch-sources.py --set moltenvk-src` fetches GitHub's archives of MoltenVK and of the parts below, at the commits
of its `ExternalRevisions/`, into `build/sources/moltenvk/` (not in the repo), pinned by their content digests in
[`third_party/sources.json`](third_party/sources.json), and `tools/build-moltenvk.sh` builds them unchanged, with
Apple's private Metal API compiled in and off by default. The app embeds the dynamic framework for the device from
`build/MoltenVK.xcframework`. The release (`github.com/KhronosGroup/MoltenVK/releases`, `tools/fetch-moltenvk.sh
--fetch`, checked against the SHA-256 in the same file) holds the same parts at the same commits.

| Part in `MoltenVK.framework` | Version | License |
|---|---|---|
| MoltenVK | `v1.4.2` @`db660224` | Apache-2.0 |
| SPIRV-Cross | `6c09849f` | Apache-2.0 |
| SPIRV-Tools | `0d6fd73c` | Apache-2.0 |
| cereal (headers only) | `a56bad8b` | BSD-3-Clause |
| SPIRV-Headers (headers only) | `29981f65` | MIT |
| Vulkan-Headers (headers only) | `v1.4.357` @`e3b1eec0` | Apache-2.0 OR MIT |

MoltenVK's own license text comes from its sources (`build/moltenvk/LICENSE`). The texts of SPIRV-Cross, SPIRV-Tools,
SPIRV-Headers and cereal, at these commits, are in [`third_party/moltenvk/licenses/`](third_party/moltenvk/licenses/)
(`tools/fetch-moltenvk.sh --licenses`, checked against their Git blobs), for the bundle. Used only to build: `vk.xml` of
Vulkan-Headers `v1.4.357` (Apache-2.0 OR MIT), read by the generator of ADR-0030, the Vulkan headers of the same version
from MoltenVK's build, against which our guest `libvulkan.so` is built, and the rest of these sources, which only build
MoltenVK. Vulkan-Tools and Volk, which only MoltenVK's demos use, are not fetched.

## Parity benchmark (ADR-0037)

Only in benchmark builds (`APK_EMBED_BENCHMARK=YES`), beside `MinatoBench.framework`: the APKs `bench-gles` and
`bench-vulkan` in `Benchmark/`, built by `tools/build-bench.sh` with the NDK r30 (`30.0.16248370`).
`MinatoBench.framework` is our own code and links ANGLE's framework (above); neither APK links libc++.

| Part | License | What for |
|---|---|---|
| `native_app_glue` | Apache-2.0 | `android_native_app_glue.c`, linked into both APKs' libraries |

Used only to build: the NDK's `glslc` (Apache-2.0), which compiles the scene's shaders for both builds.

## Time zones

Bionic reads time zones from a file in its own format. At build time, `tools/make-tzdata.py` packs them from the Mac's
IANA time zone database (`/usr/share/zoneinfo`, public domain) into `build/bionic/system/usr/share/zoneinfo/tzdata`; not
in the repo.

Android's tzdata module (`com.android.tzdata`) lies in every app build as Android 15 lays it out (the compatibility
plan's gap 21): `etc/tz/tzdata` and `etc/tz/versioned/8/tzdata` are the file above, with a `tz_version` beside each
in the tag's format (`008.001|<release>|001`, the release of the file's header); `versioned/8/tzlookup.xml` and
`telephonylookup.xml` are `system/timezone`'s `output_data/android` at the tag (Apache-2.0, generated from IANA's data
and ISO country data); `versioned/8/icu/` has ICU's four time zone resources of the same release, `zoneinfo64.res`,
`metaZones.res`, `timezoneTypes.res` and `windowsZones.res`, from the `icu-data` repository (below). ICU, libcore and
bionic then answer from one IANA release (ADR-0033 decision 9).

| Source | Pin | Licence | What ships |
|---|---|---|---|
| `unicode-org/icu-data` (GitHub) | commit `bae5ca647fa41703393dbf00240ec1b807a9c353` (2026-09-30, "ICU-23249 TZDB 2026e"), digest `494eb94e0976e8179b133ad9fb0d98fe3d23e951afc559759d2ebffd612172fe` in `third_party/sources.json` (`icu_tzdata`, list art) | Unicode License V3 (`LICENSE` at the root, fetched with the files: its notice must go with every build that ships the data); permissive, no copyleft | `tzdata/icunew/2026e/44/le/`: `zoneinfo64.res` (149,600 bytes), `metaZones.res` (43,536), `timezoneTypes.res` (21,264), `windowsZones.res` (22,064): ICU 75's resources, format 44, little-endian, in `Guest/apex/com.android.tzdata/etc/tz/versioned/8/icu/` |

The user's go for the fetch: 2026-10-08, "Ja, laden" (4 files, 234,048 bytes plus LICENSE, in Formate's chat).
A newer tz release needs these four files of that release pinned too (embed-guest-system.sh and
framework/tests/app_process_run.py name the pin to make when bionic's tzdata is of a release without them).

## ART and its libraries (ADR-0033)

AOSP sources at the tag `android-15.0.0_r1`, from `android.googlesource.com`. `tools/fetch-sources.py --set art`
downloads them to `build/sources/` (not in the repo) and checks them against the checksums in the list `art` of
[`third_party/sources.json`](third_party/sources.json). Step a) builds guest code from them with the NDK. Since step d2
every app build embeds that build's release flavor (`tools/embed-guest-system.sh`): the jars, ICU's data and the time
zone files in `Guest/apex`, the libraries as signed containers in `Guest/apex-lib64`. The debug flavor (`libartd`,
`libartbased`, `libdexfiled`, `libopenjdkd`, `libopenjdkjvmd`, `libprofiled`) and ART's tests go only into fixture
builds (`APK_EMBED_FIXTURES=YES`, `tools/embed-fixtures.sh`).

| Source | Commit | License | Which parts |
|---|---|---|---|
| `platform/art` | `bebbc3cc49f2` | Apache-2.0 (`NOTICE`); `openjdkjvm/` GPL-2.0 with Classpath Exception (`openjdkjvm/LICENSE`), as are `openjdkjvmti/` and `dt_fd_forward/`, which are not built | the runtime (`libart`), `dalvikvm`, `libopenjdkjvm` |
| `platform/libcore` | `a996d969fdd1` | Apache-2.0 with the notices in `NOTICE` (Android, Apache Harmony, ICU, KXML, W3C); `ojluni/` GPL-2.0 with Classpath Exception (`LICENSE`) | the boot jars, among them core-oj from `ojluni/`; their natives `libjavacore` and `libopenjdk` |
| `platform/external/icu` | `cf305aeb6df4` (ICU 75.1) | Unicode-3.0 and the notices in `LICENSE`; the GPL texts there cover ICU4C's Autoconf files, which are not used | ICU's libraries and data, `libicu_jni` |
| `platform/external/boringssl` | `23a87e389eb9` | OpenSSL, SSLeay, ISC and others, as in `LICENSE` | `libcrypto` of ART's module; `libcrypto_static` and libssl's static variant for Conscrypt's `libjavacrypto` (ADR-0046), tested by `boringssl-test` |
| `platform/external/conscrypt` | `f8e2d8143241` | Apache-2.0 (`LICENSE`, `MODULE_LICENSE_APACHE2`), with portions of Netty and Apache Harmony, also Apache-2.0 (`NOTICE`) | `conscrypt.jar` and `libjavacrypto` (ADR-0046 c), built into `build/art`; in the app since ADR-0033 d2 |
| `platform/external/bouncycastle` | `9be82ede5e65` | MIT, the Bouncy Castle License (`NOTICE`, `MODULE_LICENSE_BSD_LIKE`); six files AOSP added are Apache-2.0 (`crypto/digests`' `AndroidDigestFactory*` and `OpenSSLDigest`, `jce/provider/CertBlocklist`) | `bouncycastle.jar` from `repackaged/bcprov`, shrunk by R8 (ADR-0046 d), built into `build/art`; in the app since ADR-0033 d2 |
| `platform/external/okhttp` | `fdad2b22b007` | Apache-2.0 (`LICENSE.txt`, `MODULE_LICENSE_APACHE2`), okio's repackaged tree included | `okhttp.jar` from the five repackaged trees (ADR-0046 d, which took in step e), built into `build/art`; in the app since ADR-0033 d2 |
| `platform/system/ca-certificates` | `2207c6dd74f1` | no license text: AOSP's `METADATA` says `license_type: BY_EXCEPTION_ONLY` ("this package has special licensing conditions") and notes that QuoVadis's certificate policy in `files/5cf9d536.0` references the QuoVadis Certificate Holder Agreement | fetched for ADR-0046: Android 15's 145 root certificates; since step f) every app build ships them in `apex/com.android.conscrypt/cacerts`, seen at `/system/etc/security/cacerts` through the runtime's link |
| `platform/libnativehelper` | `3ca43dfe2bf4` | Apache-2.0 | `libnativehelper` (JNI helpers) |
| `platform/external/fdlibm` | `1e651e1ef2b6` | Sun Microsystems' notice (`NOTICE`) | math for libcore's `StrictMath` |
| `platform/external/expat` | `8ae3fff00472` | MIT (`COPYING`) | XML parser for libcore |
| `platform/external/zlib` | `86056a43326c` | Zlib | only its headers: ART's libraries link the guest system's `libz.so`, the NDK's zlib |
| `platform/external/lz4` | `1d69e7802438` | BSD-2-Clause for `lib/`, the only part used; the rest of the repository GPL-2.0, not used | LZ4 for ART |
| `platform/external/lzma` | `b744809d5506` | public domain | xz for ART |
| `platform/external/libcap` | `c2c4eed91fe3` | BSD-3-Clause OR GPL-2.0-only, used under BSD-3-Clause | for ART |
| `platform/external/tinyxml2` | `d3f5ba7b82e0` | Zlib | XML for ART |
| `platform/external/cpu_features` | `eca53ba6d2e9` | Apache-2.0 | CPU features for ART |
| `platform/external/vixl` | `b2b1ebaac771` | BSD-3-Clause | `src/` (aarch32 and aarch64 assemblers and decoders) and `Android.bp`, `LICENCE`: ART's compiler backend, for our own dex2oat (M6 c); `test/`, `examples/`, `tools/` and `benchmarks/` are not fetched |
| `platform/external/fmtlib` | `360e74bb8ec7` | MIT | formatting, which `libbase` links |
| `platform/system/libbase` | `68f963c62f7f` | Apache-2.0 | `libbase` |
| `platform/system/libziparchive` | `13b815f48578` | Apache-2.0 | `libziparchive` (APKs and jars) |
| `platform/system/libprocinfo` | `e2d8c69e0454` | Apache-2.0 | `libprocinfo` |
| `platform/system/unwinding` | `63e40770259e` | Apache-2.0 | `libunwindstack`, for ART's stack dumps |
| `platform/external/dlmalloc` | `f18b9ade29f2` (dlmalloc 2.8.6) | public domain (CC0-1.0, `NOTICE`) | the allocator of ART's dlmalloc spaces, compiled into `libart` |
| `platform/system/logging` | `54d3fa266d21` | Apache-2.0 (`liblog/NOTICE`) | only `liblog/include`: the headers ART and libcore compile against; `liblog.so` is Minato's own |
| `platform/system/timezone` | `fba551ac75e9` | Apache-2.0 (`NOTICE` license type; the generated data follows IANA's notice) | the format of the time zone files (ADR-0033, decision 9) is only read; `output_data/android/tzlookup.xml` and `telephonylookup.xml` ship in `Guest/apex/com.android.tzdata/etc/tz/versioned/8/` (gap 21) |

**GPL-2.0 with the Classpath Exception** (ADR-0033): core-oj from libcore's `ojluni/`, `libopenjdk` and `libopenjdkjvm`
ship as separate components, each with its license text in `Licenses/`. Their source code is not in the app. It must go
with every build that leaves the Mac, as ADR-0033 asks, and how is an open decision (`docs/STATUS.md`). ADR-0033 also
names libcore's OpenSSL parts: at this commit, OpenSSL's text in libcore is only in a test resource
(`support/src/test/java/tests/resources/x509/default.cnf`), which is not built; the OpenSSL license that ships is
BoringSSL's. Step a)'s build (`tools/art-plan.py`) puts code under this license only into core-oj.jar (`ojluni/`'s
Java), `libopenjdk` and `libopenjdkd` (its natives), and `libopenjdkjvm` and `libopenjdkjvmd` (`openjdkjvm/`).

**Ours in ART's module** (`Guest/apex-lib64`, ADR-0033): `libartpalette-system.so`
(`runtime/art/palette/palette_system.cc`), written after AOSP's `libartpalette/system/palette_fake.cc` in `platform/art`
(Apache-2.0, whose `NOTICE` ships with ART), and `heapprofd_client_api.so` (`runtime/art/heapprofd`), a stub of our own.

## Build tools for ART (ADR-0033)

Used only to build ART's Java part, not in the app. `tools/fetch-sources.py --set art` fetches them with the sources
above, the JDK checked against Adoptium's SHA-256 before it is unpacked.

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

Fetched by `tools/fetch-sources.py --set framework` at android-15.0.0_r1 (42.7 MB packed, your go). The host aapt2 and
protoc, with the protobuf, libpng, expat, libutils, libcutils, libziparchive, incfs-utils and libbuildversion they link,
are build tools in `build/` and do not ship; what the guest system ships is named here when its step builds it.

Since ADR-0039 c4, every app build ships the Java start's guest system, with these texts under "Framework sources" in
its Licenses folder:

- `system/framework/framework.jar`: frameworks/base's `core/java`, `graphics/java`, `opengl/java` and the ravenwood
  annotations, modules-utils' `java/`, build/release's ap3a values in the flag classes, the managers' generator's
  classes after the Connectivity and Wi-Fi API files, and our own classes;
- `system/framework/framework-res/`, the folder of ADR-0039 decision 8 (8,127 files): frameworks/base's `core/res`
  through our aapt2;
- `system/etc/permissions/platform.xml`, unchanged;
- `lib64/libandroid_runtime.so`: `core/jni` and libandroidfw, with static libbase and fmtlib, libcutils and libutils,
  libziparchive, libincfs-utils and libpng, and core/jni's database natives over libsqlite.so;
- `lib64/libsqlite.so`: external/sqlite's SQLite 3.44.3 with Android's functions (the compatibility plan's gap 7),
  linking ICU's libandroidicu;
- `bin/app_process64`: ours (`framework/app_process`), linking ART's libnativehelper, libsigchain and liblog.

protobuf and Soong's libbuildversion stay build tools and do not ship.

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
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | the types' set (`build/sources/frameworks_base_types`: `media/java`, `telephony/java`, `apex/jobscheduler/framework/java` and others, as `third_party/sources.json` lists them): the files `framework/sources.txt` takes from it, AudioFormat, AudioRouting and VolumeAutomation among them; and `media/java/android/media/AudioTrack.java`'s checks, states and messages, adapted into our written AudioTrack with its notice (`framework/managers/android/media/AudioTrack.java`, gap 13) |
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `packages/SystemUI/src/com/android/systemui/media/RingtonePlayer.java`'s player (its IRingtonePlayer), adapted into `framework/java/android/media/LocalRingtonePlayer.java` with its notice, and `media/java/android/media/AudioManager.java`'s hasHapticChannelsImpl in our written AudioManager (ADR-0060 e2e1's RingtoneManager step) |
| `platform/packages/modules/Media` | `579b22cdf08d` | Apache-2.0 | only `apex/framework/java/android/media/ApplicationMediaCapabilities.java` and `MediaFeature.java` (0.04 MB, `build/sources/media_module`): what an app declares it can play, taken as they are by `framework/sources.txt` (the Unity test APK gap sort, Formate's category 1) |
| `platform/packages/providers/MediaProvider` | `8a53c6d378a9` | Apache-2.0 | `apex/framework/java/android/provider/MediaStore.java` with `Column.java` and `CloudMediaProviderContract.java`, which it names (0.34 MB, `build/sources/media_provider_module`): the contract apps use to query and insert photos and videos, taken as it is by `framework/sources.txt` (ADR-0057 step e) |
| `platform/frameworks/libs/modules-utils` | `810bd8c57bb3` | Apache-2.0 | only `java/` (0.5 MB): `Preconditions`, the typed XML parsers and annotations that framework.jar's taken files use (ADR-0039 b3) |

b3.2 builds `libandroidfw` for the guest, with `libbase`, fmtlib, `libcutils`, `libutils`, `libziparchive`,
`libincfs-utils` and libpng, as static archives; they ship inside `libandroid_runtime.so` from b3.3 on. `libandroidfw`
carries Minato's patch `framework/native/patches/androidfw-directory-listing.patch` (Apache-2.0 like the file it
changes): it only changes `DirectoryAssetsProvider::ForEachFile` in `libs/androidfw/AssetsProvider.cpp` and adds three
includes there.

b3.3 builds `libandroid_runtime.so` from `core/jni`'s files for Log, AssetManager, StringBlock, XmlBlock, ApkAssets,
SystemProperties and SystemClock, and `core_jni_helpers`, unchanged, with stand-ins of ours
(`framework/native/android_runtime`). framework.jar carries Minato's patch
`framework/patches/assetmanager-framework-res-folder.patch` to `core/java/android/content/res/AssetManager.java`
(Apache-2.0 like the file it changes): only `createSystemAssetsInZygoteLocked` changes, to open framework-res's folder
and ask for no overlay idmaps. ADR-0041 a) adds `core/jni/android_animation_PropertyValuesHolder.cpp`, unchanged.
ADR-0041 a)'s GLES bindings add `core/jni`'s `android_opengl_*.cpp` (12 files),
`com_google_android_gles_jni_EGLImpl.cpp` and `com_google_android_gles_jni_GLImpl.cpp`, and `android/opengl/util.cpp`
with `poly_clip.cpp`; five of them carry Minato's patch `framework/native/patches/opengl-without-libgui.patch`
(Apache-2.0 like the files it changes): window surfaces through Minato's own function for a Surface's window, no
SurfaceTexture, `eglDupNativeFenceFDANDROID` through `eglGetProcAddress`, no init count, and no ETC1 natives (GLUtils'
bitmap natives came back with ADR-0041 a3). framework.jar takes `opengl/java`'s three packages unchanged, without `ETC1`
and `ETC1Util`. ADR-0043 c2's `framework/native/android_runtime/android_view_Surface.cpp` is Minato's, written after
`core/jni/android_view_Surface.cpp` function by function (Apache-2.0), on libnativewindow's window in place of libgui's
Surface.

The API signature files of the managers' generator (ADR-0045 decision 3), fetched the same way at the tag on your go in
the App chat. `tools/managergen` reads them and writes our own classes under their names into `build/`; the files
themselves never ship.

| Project | Commit | License | File | Size | SHA-256 |
|---|---|---|---|---|---|
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `core/api/current.txt` | 4,382,010 | `a07b0a1df1b7aeeb…` |
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `location/api/current.txt` | 53,006 | `a0495a8a32384610…` |
| `platform/packages/modules/Connectivity` | `355670c19612` | Apache-2.0 | `framework/api/current.txt` | 55,123 | `26bf1910035039b2…` |
| `platform/packages/modules/Connectivity` | `355670c19612` | Apache-2.0 | `framework-t/api/current.txt` | 20,275 | `2a4b39ab959d50a3…` |
| `platform/packages/modules/Wifi` | `a88d7fbb026b` | Apache-2.0 | `framework/api/current.txt` | 134,668 | `c10ce9a2f78634c2…` |

The module APIs' Java sources (the framework breadth, `docs/plans/2026-10-07-framework-breadth.md`), fetched by
`tools/fetch-sources.py --set framework` at the tag with the user's go of 2026-10-08 (`build/sources/location_java`,
`connectivity_java`, `wifi_java`). `tools/api-coverage.py` reads them to classify the module APIs' types, and a breadth
batch takes the plain Java ones into `framework/sources.txt`. Their folders hold tests, build files and AIDL besides the
Java; only the files a batch lists are built. `RttManager.java` in the Wi-Fi module has no license header; it lies in an
Apache-2.0 project.

| Project | Commit | License | Folders | Files | Digest |
|---|---|---|---|---|---|
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `location/java` | 96 | `f67e7935b34094ff…` |
| `platform/packages/modules/Connectivity` | `355670c19612` | Apache-2.0 | `framework`, `framework-t` | 220 | `09f780b06cb754fa…` |
| `platform/packages/modules/Wifi` | `a88d7fbb026b` | Apache-2.0 | `framework` | 320 | `23e1635aeae7342d…` |

The platform's split permissions (ADR-0045 decision 6, ADR-0039 b3.5b1), fetched the same way on your go in the App
chat. The guest reads the file unchanged at `/system/etc/permissions/platform.xml`, as Android's SystemConfig does. The
Mac's harness mounts it, and every app build ships it in its guest system since ADR-0039 c4.

| Project | Commit | License | File | Size | SHA-256 |
|---|---|---|---|---|---|
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `data/etc/platform.xml` | 18,104 | `4f1139ad764d74f3…` |

The virtual keyboard's key character map (ADR-0045 step 7c1), fetched by `tools/fetch-sources.py --set framework` on
your go in the Planung chat (`build/sources/frameworks_base_keyboards`, digest `bfebf0c1…b11a` since ADR-0068 step 7c2,
which added Generic.kcm, the map of a hardware keyboard, on the same standing rule for AOSP sources; the Planung chat's
report gave its size). framework/CMakeLists.txt builds both into libandroid_runtime as C arrays, which the input service
parses under Android's paths /system/usr/keychars/Virtual.kcm and Generic.kcm; nothing ships them as files.

| Project | Commit | License | File | Size | SHA-256 |
|---|---|---|---|---|---|
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `data/keyboards/Generic.kcm` | 15,940 | `4710a130708720a6…` |
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `data/keyboards/Virtual.kcm` | 15,716 | `6356265ec24b7eb2…` |

The vibrator HAL's AIDL (gap 1 of the compatibility plan, batch 3), fetched by `tools/fetch-sources.py --set framework`
on your go of 2026-10-29 in the lead's chat (`build/sources/hardware_interfaces_vibrator`, the entry
`hardware_interfaces_vibrator`, digest `398c80a3…c7ae`; 12 files, 31,015 bytes). tools/build-framework.sh compiles its
ten types of `android.hardware.vibrator` with aidl into framework.jar, whose AOSP VibratorInfo and Vibrator read
IVibrator's capabilities and Braking, as Android's framework.jar has them.

| Project | Commit | License | Folder | Use |
|---|---|---|---|---|
| `platform/hardware/interfaces` | `488942f82bd1` | Apache-2.0 | `vibrator/aidl/android` and `vibrator/aidl/Android.bp` | the vibrator HAL's types for framework.jar |

The media server's player AIDL (ADR-0060 e2e1), fetched by `tools/fetch-sources.py --set framework` under your standing
rule of 2026-10-11 for AOSP sources, on the lead's go of 2026-10-29 in the Grafik chat
(`build/sources/frameworks_av_media_aidl`, the entry `frameworks_av_media_aidl`, digest `bfa32365…6283`; 10 files,
26,040 bytes). tools/build-framework.sh compiles IPlayer and the VolumeShaper and Interpolator types of `android.media`
with aidl into framework.jar, whose AOSP PlayerBase and MediaPlayer take them as Android's framework.jar has them;
nothing else of frameworks/av ships.

| Project | Commit | License | Files | Use |
|---|---|---|---|---|
| `platform/frameworks/av` | `1863cb0ea5a9` | Apache-2.0 (`NOTICE`) | `media/libaudioclient/aidl/android/media/IPlayer.aidl`, eight types of `aidl/android/media` (`InterpolatorConfig`, `InterpolatorType`, `VolumeShaperConfiguration`, `VolumeShaperConfigurationOptionFlag`, `VolumeShaperConfigurationType`, `VolumeShaperOperation`, `VolumeShaperOperationFlag`, `VolumeShaperState`) and `NOTICE` | PlayerBase's IPlayer and its parcelables for framework.jar |

SQLite and Android's functions for it (the compatibility plan's gap 7), fetched by `tools/fetch-sources.py --set
framework` on your go of 2026-10-07 in the chat Formate (`build/sources/sqlite`, the entry `sqlite`, digest
`6ba2eee5…f850`). framework/CMakeLists.txt builds `lib64/libsqlite.so` from them, as `dist/Android.bp` builds libsqlite
for the android target: SQLite 3.44.3's amalgamation, the version that android-15.0.0_r1's release flag
RELEASE_PACKAGE_LIBSQLITE3 picks, with android/'s functions linked in whole (libsqlite3_android). Every app build ships
it in its guest system, with these texts under "Framework sources". libandroid_runtime.so's database natives
(`core/jni`'s `android_database_*.cpp`, unchanged) link it, and framework.jar takes `core/java/android/database/sqlite`
unchanged.

| Project | Commit | License | File | Size | SHA-256 |
|---|---|---|---|---|---|
| `platform/external/sqlite` | `af6bb6a379e1` | public domain (SQLite's blessing, `dist/NOTICE`) | `dist/sqlite-autoconf-3440300/sqlite3.c` | 8,925,885 | `f191da49b70e8c5d…` |
| `platform/external/sqlite` | `af6bb6a379e1` | public domain | `dist/sqlite-autoconf-3440300/sqlite3.h` | 638,754 | `b68bd370cce54abb…` |
| `platform/external/sqlite` | `af6bb6a379e1` | public domain | `dist/sqlite-autoconf-3440300/sqlite3ext.h` | 38,149 | `b184dd1586d93513…` |
| `platform/external/sqlite` | `af6bb6a379e1` | Apache-2.0 (`android/NOTICE`) | `android/sqlite3_android.cpp` | 18,716 | `e5ca750bf21f06e9…` |
| `platform/external/sqlite` | `af6bb6a379e1` | Apache-2.0 | `android/sqlite3_android.h` | 951 | `4fddda110375d61c…` |
| `platform/external/sqlite` | `af6bb6a379e1` | Apache-2.0 | `android/PhoneNumberUtils.cpp` | 16,172 | `9b468e524fe2450f…` |
| `platform/external/sqlite` | `af6bb6a379e1` | Apache-2.0 | `android/PhoneNumberUtils.h` | 1,124 | `83e6b3f1d2506485…` |
| `platform/external/sqlite` | `af6bb6a379e1` | Apache-2.0 | `android/OldPhoneNumberUtils.cpp` | 7,782 | `0822bf88d7c957b7…` |

The fetch also takes `LICENSE`, `dist/NOTICE`, `android/NOTICE` and the two `Android.bp` files, which are read, not
built.

The default MIME map behind `android.webkit.MimeTypeMap` and `URLConnection.guessContentTypeFromName` (framework breadth
batch B1), fetched by `tools/fetch-sources.py --set framework` on your go in the chat Planung (2026-10-10;
`build/sources/mime_map` and `build/sources/mime_support`, the entries `mime_map` and `mime_support`, digests
`ea842014…d16d` and `a08dd24e…907b3d7c`). 86 KB; `DefaultMimeMapFactory` reads the three tables, which
`tools/build-framework.sh` puts into framework.jar as `res/…` as Android's `mimemap-res.jar` does.

| Project | Commit | License | File | Size | SHA-256 |
|---|---|---|---|---|---|
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `mime/java/android/content/type/DefaultMimeMapFactory.java` | 4,329 | `f8a6b3fda9f1b581…` |
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `mime/java-res/android.mime.types` | 4,586 | `f1d10e57da386f92…` |
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `mime/java-res/vendor.mime.types` | 1,476 | `626e767be0115aa7…` |
| `platform/external/mime-support` | `fa5a0ada898e` | public domain | `mime.types` | 74,975 | `fb5d3dced49ceecd…` |
| `platform/external/mime-support` | `fa5a0ada898e` | public domain | `LICENSE` | 442 | `ce7ee91d78762c68…` |

The evidence for "public domain": the project's `LICENSE` is a Debian copyright file that states `Copyright:
public-domain`, `License: ad-hoc`, and "It contains public information compiled from around the 'net and many people",
`METADATA` names `license_type: UNENCUMBERED` (Debian's media-types package, version `0cde41f063cf`, upgraded
2024-07-12), the project's `Android.bp` declares the license kind `legacy_unencumbered`, and the file
`MODULE_LICENSE_PUBLIC_DOMAIN` is empty and present. Nothing of it is copied into our source files: the build puts the
tables into framework.jar after the same minimizing step as Android's `debian.mime.types.minimized` genrule (comments
stripped, whitespace normalized, lines without an extension dropped).

TagSoup 1.2.1, the HTML parser behind `android.text.Html.fromHtml` (framework breadth, step (a) after batch B4), fetched
by `tools/fetch-sources.py --set framework` on your go in the chat Planung (2026-10-11: "lad herunter was auch immer du
brauchst, aber wirklich nur wenn wir das brauchen, um alle apps laufen zu lassen"; `build/sources/tagsoup`, the entry
`tagsoup`, digest `0b418969…e428`). 0.37 MB, 15 source files of `platform/external/tagsoup` at android-15.0.0_r1 (commit
`5c387116e235`) and its `LICENSE`; the tests, the `jaxp` folder and the command line tools' extras are left out. The
sources are compiled into framework.jar as AOSP does (`org.ccil.cowan.tagsoup`).

| Project | Commit | License | File | Size | SHA-256 |
|---|---|---|---|---|---|
| `platform/external/tagsoup` | `5c387116e235` | Apache-2.0 | `LICENSE` | 11,357 | `c71d239df91726fc…` |
| `platform/external/tagsoup` | `5c387116e235` | Apache-2.0 | `src/org/ccil/cowan/tagsoup/*.java` (15 files, e.g. `Parser.java`, `HTMLSchema.java`) | 263,377 | see `third_party/sources.json` |

The evidence for the license: every file starts with "This file is part of TagSoup and is Copyright 2002-2008 by John
Cowan. TagSoup is licensed under the Apache License, Version 2.0", the project's `LICENSE` is the Apache License 2.0,
`MODULE_LICENSE_APACHE2` is present, and `METADATA` says `license_type: NOTICE`. Android.bp also lists AFL-3.0 and
"legacy unencumbered" for the project, both permissive; there is no GPL file or header in the files we take.

stb_vorbis 1.22 (Sean Barrett, `nothings/stb`), SoundPool's Ogg Vorbis decoder (the compatibility plan's gap 13, step 4
of `docs/plans/2026-10-30-soundpool-and-audio-codecs.md`), fetched by `tools/fetch-sources.py --set framework` on the
user's go, relayed by the lead on 2026-10-30, built into `libandroid_runtime.so` without stdio and the push-data API:

| Project | Commit | License | File | Size | SHA-256 |
|---|---|---|---|---|---|
| `nothings/stb` | `1ee679ca2ef7` | MIT or public domain (the user's choice) | `LICENSE` | 2,510 | `bebfe904b1430165…` |
| `nothings/stb` | `1ee679ca2ef7` | MIT or public domain (the user's choice) | `stb_vorbis.c` | 192,790 | `4c7cb2ff1f7011e9…` |

The evidence for the license: the file's head says "public domain" and "See end of file for license information", and
its end and `LICENSE` offer "ALTERNATIVE A - MIT License" and "ALTERNATIVE B - Public Domain (www.unlicense.org)".

## Graphics sources (ADR-0043 a)

Fetched by `tools/fetch-sources.py --set graphics` at android-15.0.0_r1 (22.6 MB packed, your go; the headers of
frameworks/native's `include/android` and `opengl/include` and libhardware's `hardware.h` 0.25 MB more, your go of
2026-10-02). Since ADR-0039 c4, every app build ships the 13 libraries ADR-0043 a1 builds in its guest system's `lib64`,
and libjnigraphics with them since ADR-0041 a5, with these texts under "Graphics sources" in its Licenses folder:
libhwui, libminikin, libft2, libharfbuzz_ng and libharfbuzz_subset, libpng, libjpeg, libjpegdecoder, libjpegencoder and
libultrahdr, libimage_io, libcutils and server_configurable_flags. frameworks/native, vulkan-headers, frameworks/av,
hardware/interfaces, libhardware and googletest's `gtest_prod.h` are headers only. FreeType's and the IJG's credits
stand in their components' license lines.

| Project | Commit | License | What we take |
|---|---|---|---|
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `libs/hwui` (the host flavor, its graphics JNI and the device's ABitmap C API, `apex/android_bitmap.cpp`), `libs/hostgraphics`; `data/fonts` is under "Guest fonts" below |
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 | `native/graphics/jni`, libjnigraphics for NDK apps (ADR-0041 a5): 6 files (`Android.bp`, `aassetstreamadaptor.cpp` and `.h`, `bitmap.cpp`, `imagedecoder.cpp`, `libjnigraphics.map.txt`; `fuzz/` left out), 33,559 bytes, SHA-256 `31cc3f819a049394…`, fetched as `frameworks_base_jnigraphics` on your go of 2026-10-05 in the Grafik chat and built with patch 0010 |
| `platform/external/skia` | `0fe593a9a878` | BSD-3-Clause (`LICENSE`) | the CPU backend: `include`, `src` without the GPU sources, `modules/skcms`, `client_utils/android` |
| `platform/frameworks/minikin` | `b41e852cee7f` | Apache-2.0 | text layout, with its C++ hyphenator (the Rust one is left out) |
| `platform/external/harfbuzz_ng` | `9a50c421c9ca` | Old MIT (`COPYING`); the GPL texts in `LICENSE` cover files that are not compiled | text shaping (`src`), and the subsetter Skia's PDF uses |
| `platform/external/freetype` | `5ab8c8be9c2c` | FreeType License (`docs/FTL.TXT`), taken of the FTL or GPL-2.0 it offers; the FTL asks for a credit in our documentation | font rasterizer (`src`, `include`) |
| `platform/external/libjpeg-turbo` | `1e113e5593fa` | IJG, BSD-3-Clause and zlib (`LICENSE.md`) | JPEG codec |
| `platform/external/webp` | `c3a8e58734bd` | BSD-3-Clause (`COPYING`), with Google's patent grant (`PATENTS`) | WebP codec (`src`, `sharpyuv`) |
| `platform/external/wuffs-mirror-release-c` | `479408ccc13b` | Apache-2.0 | GIF decoder |
| `platform/external/giflib` | `b771f86bcb3d` | MIT (`NOTICE`) | GIF decoding for hwui's Movie (`jni/GIFMovie.cpp`), statically in libhwui: fetched as `giflib` on your go of 2026-10-11 in the Grafik chat, 0.03 MB, digest `4ee3a007…eba9` |
| `platform/external/libultrahdr` | `448f96ed8315` | Apache-2.0 | JPEG gain maps |
| `platform/external/image_io` | `37e2347000d7` | Apache-2.0 | XMP and container parsing for libultrahdr |
| `platform/external/modp_b64` | `d88d4daed951` | BSD-style (`LICENSE`) | base64 for image_io |
| `platform/frameworks/native` | `f7274fca5e36` | Apache-2.0; in `opengl/include` the Khronos headers MIT (`MODULE_LICENSE_MIT`), ANGLE's `EGL/eglext_angle.h` and `EGL/Platform.h` BSD-3-Clause (as ANGLE above), `GLES3/gl3ext.h` SGI Free Software License B 2.0 | headers (`include/private`, `include/android`, `opengl/include`, libgui's `TraceUtils.h`, nativebase, nativedisplay, nativewindow, binder NDK, math), `libs/ui`'s types, `libs/arect`; `libs/permission`'s `AttributionSourceState.aidl` as the Java aidl writes from it (framework/java, ADR-0045 gap 13a; read at android-15.0.0_r1, not fetched); libinput's `libs/input/Input.cpp` and `VelocityTracker.cpp` with their headers and ftl's, 12 files (`build/sources/frameworks_native_input`, digest `914d91f6…136d`) for MotionEvent's and VelocityTracker's natives (ADR-0041 decision 5 as changed, ADR-0045 step 7b1); libinput's `libs/input/KeyCharacterMap.cpp` and `include/input/KeyCharacterMap.h` (`build/sources/frameworks_native_keys`, digest `5401db5a…1c18`) for KeyCharacterMap's natives, and `libs/input/Keyboard.cpp`'s meta-state functions as an excerpt with its notice (`framework/native/android_runtime/keyboard_meta_state.cpp`; read at android-15.0.0_r1, not fetched) (ADR-0045 step 7c1); libETC1's `opengl/libs/ETC1/etc1.cpp` (`build/sources/frameworks_native_etc1`, digest `e281b9c2…eef2`, your go of 2026-10-13) for ETC1's natives in libandroid_runtime (the graphics breadth's G1) |
| `platform/external/vulkan-headers` | `d3602c8e1b0b` | Apache-2.0 or MIT (`LICENSE.md`) | the 12 C headers hwui includes |
| `platform/frameworks/av` | `1863cb0ea5a9` | Apache-2.0 | `media/ndk/include` |
| `platform/hardware/interfaces` | `488942f82bd1` | Apache-2.0 (`NOTICE`) | the AIDL of graphics common and composer3, for their enum headers |
| `platform/hardware/libhardware` | `58faa60aadb6` | Apache-2.0 (`NOTICE`) | `hardware.h`, which libui's `PixelFormat.h` includes |
| `platform/system/server_configurable_flags` | `3b6d3e8d7d20` | Apache-2.0 | `libflags` |
| `platform/external/googletest` | `c05eedac6f5e` | BSD-3-Clause | `gtest_prod.h`, which minikin includes |

## Guest fonts (ADR-0043 test 2)

From the graphics set above. The guest's `/system/fonts` holds these 4 fonts (6.8 MB), and its `/system/etc` our cuts of
AOSP's two font lists, `graphics/fonts/fonts.xml` and `font_fallback.xml` (Apache-2.0, as the originals; each names its
source). `tools/build-bionic.sh` puts them into `build/bionic/system` (`graphics/fonts/fonts.cmake`), and the app
carries them in `Guest/system` (`tools/embed-guest-system.sh`).

| Project | Commit | License | What we take |
|---|---|---|---|
| `platform/frameworks/base` | `4e43ad12e1b2` | Apache-2.0 (`data/fonts/NOTICE`) | from `data/fonts`: `DroidSansMono.ttf`, `DroidSansFallbackFull.ttf`, and `fonts.xml` and `font_fallback.xml`, which ours are cut from |
| `platform/external/roboto-fonts` | `4029d6e45cb3` | Apache-2.0 (`NOTICE`) | `Roboto-Regular.ttf`, `RobotoStatic-Regular.ttf` |

## Test 2 with Godot (ADR-0042)

The engine of test 2's app and the tool its export needs, in `build/godot/`, not in the app. `tools/fetch-godot.py`
fetches them and checks every file against its pin in `third_party/sources.json` before it unpacks; Godot's license
texts are kept in `third_party/godot/`. The twin, Godot's iOS template, goes into benchmark builds for devices only
(ADR-0042 c), as `Frameworks/MinatoGodot.framework`, which is this section's rule for the bundle.

| Tool | Version | License |
|---|---|---|
| Godot editor for macOS (`Godot_v4.7.2-stable_macos.universal.zip`), self-contained in `build/godot/`, which exports the test APKs | `4.7.2-stable` (`ed1daf0bf001`), SHA-512 pinned | MIT (`third_party/godot/LICENSE.txt`; the third parties in `COPYRIGHT.txt`) |
| Godot's export templates (`Godot_v4.7.2-stable_export_templates.tpz`), of which only `android_release.apk` and `ios.zip` are unpacked | `4.7.2-stable`, SHA-512 pinned | MIT, as above |
| Android SDK build-tools (`build-tools_r35.0.1_macosx.zip`) in `build/godot/sdk/`, for `apksigner`, which the export without Gradle needs | `35.0.1`, Google's SHA-1 and our SHA-256 pinned | Android Software Development Kit License |

## Test 2's APKs (ADR-0042)

The two test APKs that `tools/build-godot.sh` exports at test time are never kept in the repo. A benchmark build with
`APK_EMBED_GODOT=YES` (`tools/benchmark.sh --scene godot`) carries both in `Benchmark/` (`tools/embed-benchmark.sh`),
and every other build removes them, so this section's rule for the bundle is the Vulkan APK's path. They hold what
Godot's Android template holds; Swappy stays inside `libgodot_android.so` also while `godot-bench` turns it off. The
Apache-2.0 components take the repository's `LICENSE` as their text.

| What the APKs hold | License |
|---|---|
| Godot's Android template (`android_release.apk`), the engine of the test APKs | MIT (`third_party/godot/LICENSE.txt`; the third parties in `COPYRIGHT.txt`) |
| AndroidX libraries in the test APKs' dex | Apache-2.0 |
| Kotlin's standard library in the test APKs' dex | Apache-2.0 |
| Swappy, the Android Game Development Kit's frame pacing, in `libgodot_android.so` | Apache-2.0 |

## Build tools for the formats (M5)

Mac tools for M5's test cases and its `.aab` tool, in `build/tools/`, not in the app; pinned in
`third_party/sources.json`, fetched on your go of 2026-10-04 in the chat Formate.

| Tool | Version | License |
|---|---|---|
| bundletool (`bundletool-all-1.18.3.jar`) in `build/tools/bundletool/`, run with the JDK above and our aapt2 (`--aapt2 build/aapt2/aapt2`); the aapt2 binaries inside the jar are never run | `1.18.3` (`586a43a45071`), SHA-256 `a099cfa1543f` | Apache-2.0 |
| Google's Play libraries for pad-test (ADR-0050 test 3): asset-delivery 2.2.2 and feature-delivery 2.1.0 with core-common 2.0.3, play-services-basement 18.3.0 and play-services-tasks 18.1.0, as AARs in `~/.android/minato-play-libraries/` (`tools/fetch-play-libraries.py`), outside the repository and never in the app; only pad-test's APK, built on this Mac, carries their code, which nothing here reads | pinned in `third_party/sources.json` (`play-libraries`), each AAR by its SHA-256 | Play Core Software Development Kit Terms of Service with Google's APIs Terms of Service (asset-delivery, feature-delivery, core-common); Android Software Development Kit License (play-services-basement, play-services-tasks); both accepted by you on 2026-10-07 in the chat Formate |

## References without adopted code

- **ARTBox** (MIT, `fd48d48`): The list of Bionic sources and their commits for `android-15.0.0_r1` served as the
  starting point; the commits have been checked against the tag on `android.googlesource.com`. No code is adopted.
- **Certificate from SideStore and AltStore** (ADR-0023). Only read:
  - which URL the export deep link has and which placeholders it replaces;
  - which markers the stores write into the Info.plist;
  - how SideStore builds its `.p12`.

  No code is adopted. The PKCS#12 reader and the callback parser are our own code, written from RFC 7292 and from the
  measured behavior.

  | Project | Version | License |
  |---|---|---|
  | LiveContainer | `main` @`4dbe0f9a` (3.8.10) | AGPL-3.0 |
  | SideStore | `0.7.0-alpha` @`6032424`, `develop` @`0dd743f` | AGPL-3.0 |
  | AltStore Classic | `classic` @`fafd76ee` (2.3) | AGPL-3.0 |
  | AltSign | `0d3c1a3c` | no license file |
  | SideSign | `6b68651` | no license file |
  | CodeSignKit | `d0c6771` | not stated |
