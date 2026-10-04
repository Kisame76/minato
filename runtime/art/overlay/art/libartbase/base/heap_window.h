// Minato (ADR-0033 decision 3): the heap window. ART keeps a reference to an object as its address in 32 bits and
// wants its heap below 4 GiB, where iOS maps nothing. So every low_4gb mapping of MemMap lies in one window that
// ends gHeapWindowSize bytes (4 GiB) above gHeapWindowBase, which is aligned to 4 GiB, and a reference is an object's
// offset from that base. The window is all 4 GiB, or their upper 2 GiB where the system has no room for all of them
// (heap_window_ranges.h). The low 32 bits of an address in the window are that offset already: encoding keeps them,
// decoding puts the base in front, and the offset 0 stays null, as the window's first page is never mapped.

#ifndef ART_LIBARTBASE_BASE_HEAP_WINDOW_H_
#define ART_LIBARTBASE_BASE_HEAP_WINDOW_H_

#include <stddef.h>
#include <stdint.h>

#include "macros.h"

namespace art {

// Set once, at the first low_4gb mapping or the first question for the window (MemMap::HeapWindowBegin), before
// any object exists. Visible to every library of ART, which all decode references inline.
extern __attribute__((visibility("default"))) uintptr_t gHeapWindowBase;
extern __attribute__((visibility("default"))) size_t gHeapWindowSize;
// The window's first address past its first page, which is never mapped.
extern __attribute__((visibility("default"))) uintptr_t gHeapWindowFirst;

// The address an encoded reference stands for: null for 0, else the base and the offset.
ALWAYS_INLINE inline uintptr_t HeapWindowDecode(uint32_t reference) {
  return reference != 0u ? (gHeapWindowBase | reference) : 0u;
}

// A reference to the object at `address`, which is null or in the window: its offset.
ALWAYS_INLINE inline uint32_t HeapWindowEncode(uintptr_t address) {
  return static_cast<uint32_t>(address);
}

// Whether `address` is one an object can have: in the window, past its first page, and aligned to `alignment`.
ALWAYS_INLINE inline bool HeapWindowHolds(uintptr_t address, size_t alignment) {
  return address >= gHeapWindowFirst && address - gHeapWindowBase < gHeapWindowSize &&
         (address & (alignment - 1u)) == 0u;
}

}  // namespace art

#endif  // ART_LIBARTBASE_BASE_HEAP_WINDOW_H_
