# Third-party code

What Minato adopts from others, with license and version. Project license: Apache-2.0 (ADR-0017). GPL and AGPL code is not adopted.

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
