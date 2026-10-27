// Minato (ADR-0053 decision 3): the heap window's null guard. A reference is an object's 32-bit offset from the
// window base B (base/heap_window.h); null is 0, and the unconditional decode turns it into B, so a null dereference
// faults in the guard page [B, B + page) that ReserveHeapWindow keeps unmapped (base/heap_window_ranges.h). ART's
// implicit null check must recognize that fault and give the NPE path the offset into the absent object, exactly as
// on Android, where null is 0 and a fault at [0, page) already is the offset. The select-decode fallback and Android
// both fault at [0, page), so both ranges count; `base` is 0 before the window is reserved, when only [0, page)
// applies. Plain C++ without the rest of ART, so that runtime/tests/heap_window_null_test.cpp checks it on the Mac.

#ifndef ART_LIBARTBASE_BASE_HEAP_WINDOW_NULL_H_
#define ART_LIBARTBASE_BASE_HEAP_WINDOW_NULL_H_

#include <stddef.h>
#include <stdint.h>

namespace art {

// Whether a fault at `fault_address` is a null dereference ART can throw NullPointerException for, given the heap
// window base `base` and the runtime `page_size`. True for a fault in Android's zero page [0, page_size) (also the
// select-decode fallback, where null stays 0) and in Minato's guard page [base, base + page_size) (the unconditional
// decode, where null is B). On a hit `*offset` gets the faulting offset into the absent object — the fault address
// for the zero page, the address less `base` for the guard page — which is what the NPE-from-signal path expects.
inline bool HeapWindowNullFault(uintptr_t fault_address, uintptr_t base, size_t page_size, uintptr_t* offset) {
  if (fault_address < page_size) {  // the zero page: Android's null, or the select decode's null (both 0)
    *offset = fault_address;
    return true;
  }
  const uintptr_t delta = fault_address - base;  // the guard page: the unconditional decode's null (B)
  if (base != 0u && delta < page_size) {
    *offset = delta;
    return true;
  }
  return false;
}

}  // namespace art

#endif  // ART_LIBARTBASE_BASE_HEAP_WINDOW_NULL_H_
