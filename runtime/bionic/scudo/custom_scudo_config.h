/*
 * Copyright (C) 2023 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

//===-- custom_scudo-config.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Minato (ADR-0035, amended): AOSP's config/custom_scudo_config.h from platform/external/scudo at
// 67dd481e24c770b9f416f61844c2d015f125c134 (third_party/sources.json), with two values of
// AndroidNormalConfig::Primary changed:
//   - RegionSizeLog 27, a region of 128 MiB per size class, instead of 28 (256 MiB);
//   - EnableContiguousRegions false instead of Scudo's default true: a size class reserves its
//     region when it is first used, instead of all 33 regions in one piece at the first malloc.
// The primary then takes at most 33 x 128 MiB = 4.1 GiB of address space, in pieces of 128 MiB,
// instead of 8.25 GiB in one piece, which an iPhone app without increased-memory-limit cannot
// reserve (only 7.84 GiB of VA; ADR-0012 spike S2). 4.1 GiB fits even that budget beside ART's heap
// window (2 GiB without the entitlement), so both install paths keep working, while a single size
// class now holds 128 MiB -- the amendment's reason: at the earlier 64 MiB a Ren'Py 7.8.7 game ran a
// class out of its region during init ("Can't populate more pages for size class 800 / 1104 / 1648 /
// 7120", fixtures/suite/scudo-region-test.c). EnableRandomOffset applies only to contiguous regions,
// so it has no effect. runtime/bionic/CMakeLists.txt puts this folder before AOSP's config/ for
// bionic_libscudo.

#pragma once

// Use a custom config instead of the config found in allocator_config.h
namespace scudo {

struct AndroidNormalSizeClassConfig {
#if SCUDO_WORDSIZE == 64U
  static const uptr NumBits = 7;
  static const uptr MinSizeLog = 4;
  static const uptr MidSizeLog = 6;
  static const uptr MaxSizeLog = 16;
  static const u16 MaxNumCachedHint = 13;
  static const uptr MaxBytesCachedLog = 13;

  static constexpr uptr Classes[] = {
      0x00020, 0x00030, 0x00040, 0x00050, 0x00060, 0x00070, 0x00090, 0x000b0,
      0x000c0, 0x000e0, 0x00120, 0x00160, 0x001c0, 0x00250, 0x00320, 0x00450,
      0x00670, 0x00830, 0x00a10, 0x00c30, 0x01010, 0x01210, 0x01bd0, 0x02210,
      0x02d90, 0x03790, 0x04010, 0x04810, 0x05a10, 0x07310, 0x08210, 0x10010,
  };
  static const uptr SizeDelta = 16;
#else
  static const uptr NumBits = 8;
  static const uptr MinSizeLog = 4;
  static const uptr MidSizeLog = 7;
  static const uptr MaxSizeLog = 16;
  static const u16 MaxNumCachedHint = 14;
  static const uptr MaxBytesCachedLog = 13;

  static constexpr uptr Classes[] = {
      0x00020, 0x00030, 0x00040, 0x00050, 0x00060, 0x00070, 0x00080, 0x00090,
      0x000a0, 0x000b0, 0x000c0, 0x000e0, 0x000f0, 0x00110, 0x00120, 0x00130,
      0x00150, 0x00160, 0x00170, 0x00190, 0x001d0, 0x00210, 0x00240, 0x002a0,
      0x00330, 0x00370, 0x003a0, 0x00400, 0x00430, 0x004a0, 0x00530, 0x00610,
      0x00730, 0x00840, 0x00910, 0x009c0, 0x00a60, 0x00b10, 0x00ca0, 0x00e00,
      0x00fb0, 0x01030, 0x01130, 0x011f0, 0x01490, 0x01650, 0x01930, 0x02010,
      0x02190, 0x02490, 0x02850, 0x02d50, 0x03010, 0x03210, 0x03c90, 0x04090,
      0x04510, 0x04810, 0x05c10, 0x06f10, 0x07310, 0x08010, 0x0c010, 0x10010,
  };
  static const uptr SizeDelta = 16;
#endif
};

typedef TableSizeClassMap<AndroidNormalSizeClassConfig>
    AndroidNormalSizeClassMap;

#if defined(__LP64__)
static_assert(AndroidNormalSizeClassMap::usesCompressedLSBFormat(), "");
#endif

struct HostConfig {
  static const bool MaySupportMemoryTagging = false;

  template <class A> using TSDRegistryT = TSDRegistryExT<A>; // Exclusive TSD

  struct Primary {
    using SizeClassMap = AndroidNormalSizeClassMap;
#if SCUDO_CAN_USE_PRIMARY64
    static const uptr RegionSizeLog = 30U;
    typedef u32 CompactPtrT;
    static const uptr CompactPtrScale = SCUDO_MIN_ALIGNMENT_LOG;
    static const uptr GroupSizeLog = 26U;
    static const bool EnableRandomOffset = false;
    static const uptr MapSizeIncrement = 1UL << 18;
#else
    static const uptr RegionSizeLog = 18U;
    static const uptr GroupSizeLog = 18U;
    typedef uptr CompactPtrT;
#endif
    static const s32 MinReleaseToOsIntervalMs = -1;
    static const s32 MaxReleaseToOsIntervalMs = 10000;
    static const s32 DefaultReleaseToOsIntervalMs = 10000;
  };
#if SCUDO_CAN_USE_PRIMARY64
  template <typename Config> using PrimaryT = SizeClassAllocator64<Config>;
#else
  template <typename Config> using PrimaryT = SizeClassAllocator32<Config>;
#endif

  struct Secondary {
    struct Cache {
      static const u32 EntriesArraySize = 1024U;
      static const u32 QuarantineSize = 32U;
      static const u32 DefaultMaxEntriesCount = 1024U;
      static const uptr DefaultMaxEntrySize = 1UL << 30;
      static const s32 MinReleaseToOsIntervalMs = -1;
      static const s32 MaxReleaseToOsIntervalMs = 10000;
      static const s32 DefaultReleaseToOsIntervalMs = 10000;
    };
    template <typename Config> using CacheT = MapAllocatorCache<Config>;
  };

  template <typename Config> using SecondaryT = MapAllocator<Config>;
};

struct AndroidNormalConfig {
#if defined(__aarch64__)
  static const bool MaySupportMemoryTagging = true;
#else
  static const bool MaySupportMemoryTagging = false;
#endif
  template <class A>
  using TSDRegistryT = TSDRegistrySharedT<A, 8U, 2U>; // Shared, max 8 TSDs.

  struct Primary {
    using SizeClassMap = AndroidNormalSizeClassMap;
#if SCUDO_CAN_USE_PRIMARY64
    static const uptr RegionSizeLog = 27U; // Minato (ADR-0035, amended): 28U in AOSP, 26U before
    static const bool EnableContiguousRegions = false; // Minato (ADR-0035)
    typedef u32 CompactPtrT;
    static const uptr CompactPtrScale = SCUDO_MIN_ALIGNMENT_LOG;
    static const uptr GroupSizeLog = 20U;
    static const bool EnableRandomOffset = true;
    static const uptr MapSizeIncrement = 1UL << 18;
#else
    static const uptr RegionSizeLog = 18U;
    static const uptr GroupSizeLog = 18U;
    typedef uptr CompactPtrT;
#endif
    static const s32 MinReleaseToOsIntervalMs = -1;
    static const s32 MaxReleaseToOsIntervalMs = 1000;
    static const s32 DefaultReleaseToOsIntervalMs = 1000;
  };
#if SCUDO_CAN_USE_PRIMARY64
  template <typename Config> using PrimaryT = SizeClassAllocator64<Config>;
#else
  template <typename Config> using PrimaryT = SizeClassAllocator32<Config>;
#endif

  struct Secondary {
    struct Cache {
      static const u32 EntriesArraySize = 256U;
      static const u32 QuarantineSize = 32U;
      static const u32 DefaultMaxEntriesCount = 32U;
      static const uptr DefaultMaxEntrySize = 2UL << 20;
      static const s32 MinReleaseToOsIntervalMs = -1;
      static const s32 MaxReleaseToOsIntervalMs = 1000;
      static const s32 DefaultReleaseToOsIntervalMs = 0;
    };
    template <typename Config> using CacheT = MapAllocatorCache<Config>;
  };

  template <typename Config> using SecondaryT = MapAllocator<Config>;
};

struct AndroidLowMemoryConfig {
#if defined(__aarch64__)
  static const bool MaySupportMemoryTagging = true;
#else
  static const bool MaySupportMemoryTagging = false;
#endif
  template <class A> using TSDRegistryT = TSDRegistrySharedT<A, 1U, 1U>;

  struct Primary {
    // Use the same size class map as the normal config.
    using SizeClassMap = AndroidNormalSizeClassMap;
#if SCUDO_CAN_USE_PRIMARY64
    static const uptr RegionSizeLog = 28U;
    typedef u32 CompactPtrT;
    static const uptr CompactPtrScale = SCUDO_MIN_ALIGNMENT_LOG;
    static const uptr GroupSizeLog = 18U;
    static const bool EnableRandomOffset = true;
    static const uptr MapSizeIncrement = 1UL << 18;
#else
    static const uptr RegionSizeLog = 20U;
    static const uptr GroupSizeLog = 20U;
    typedef uptr CompactPtrT;
#endif
    static const s32 MinReleaseToOsIntervalMs = 100;
    static const s32 MaxReleaseToOsIntervalMs = 1000;
  };
#if SCUDO_CAN_USE_PRIMARY64
  template <typename Config> using PrimaryT = SizeClassAllocator64<Config>;
#else
  template <typename Config> using PrimaryT = SizeClassAllocator32<Config>;
#endif

  struct Secondary {
    // TODO(cferris): After secondary caching tuned, re-add a cache config.
    template <typename Config> using CacheT = MapAllocatorNoCache<Config>;
  };

  template <typename Config> using SecondaryT = MapAllocator<Config>;
};

#if defined(__ANDROID__)

#if defined(SCUDO_LOW_MEMORY)
typedef AndroidLowMemoryConfig Config;
#else
typedef AndroidNormalConfig Config;
#endif

#else

typedef HostConfig Config;

#endif

typedef Config DefaultConfig;

} // namespace scudo
