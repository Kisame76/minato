// Minato (ADR-0033 decision 3): where MemMap's low_4gb mappings go in the heap window of heap_window.h, and which of
// its ranges they hold. Plain C++ without the rest of ART, so that runtime/tests/heap_window_test.cpp checks it on
// the Mac; MemMap keeps one instance under its window lock and maps and unmaps by its answers.

#ifndef ART_LIBARTBASE_BASE_HEAP_WINDOW_RANGES_H_
#define ART_LIBARTBASE_BASE_HEAP_WINDOW_RANGES_H_

#include <stddef.h>
#include <stdint.h>

#include <iterator>
#include <map>

namespace art {

class HeapWindowRanges {
 public:
  // The window [base, base + size). Its first and its last page are never given out: no object has the offset 0,
  // which is null, and no mapping ends at 2^32, whose 32 bits are 0 as well (End() of a MemMap at the top).
  HeapWindowRanges(uintptr_t base, size_t size, size_t page_size)
      : first_(base + page_size), end_(base + size - page_size) {}

  // The part of the window that mappings may use: [First(), End()).
  uintptr_t First() const { return first_; }
  uintptr_t End() const { return end_; }

  // Whether [begin, begin + length) lies in that part.
  bool Holds(uintptr_t begin, size_t length) const {
    return begin >= first_ && begin <= end_ && length <= end_ - begin;
  }

  // Where a new mapping of `length` bytes goes: at `hint` where the window has room for it there, as mmap takes a
  // hint, else at the top of the highest gap that holds it. So the window fills from the top, and every object has
  // an offset above 2^31 from the start, where a reference that is sign-extended somewhere shows at once. 0 when no
  // gap holds it.
  uintptr_t Place(uintptr_t hint, size_t length) const {
    if (length == 0u || length > end_ - first_) {
      return 0u;
    }
    if (hint != 0u && Holds(hint, length) && IsFree(hint, length)) {
      return hint;
    }
    uintptr_t top = end_;
    for (auto it = ranges_.rbegin(); it != ranges_.rend(); ++it) {
      if (it->second <= top && top - it->second >= length) {
        return top - length;
      }
      if (it->first < top) {
        top = it->first;
      }
    }
    return top - first_ >= length ? top - length : 0u;
  }

  // [begin, begin + length) is mapped now.
  void Add(uintptr_t begin, size_t length) { ranges_.emplace(begin, begin + length); }

  // [begin, begin + length) is free again: every range that overlaps it keeps only what lies outside it.
  void Remove(uintptr_t begin, size_t length) {
    const uintptr_t end = begin + length;
    auto it = ranges_.upper_bound(begin);
    if (it != ranges_.begin()) {
      --it;
    }
    while (it != ranges_.end() && it->first < end) {
      const uintptr_t range_begin = it->first;
      const uintptr_t range_end = it->second;
      if (range_end <= begin) {
        ++it;
        continue;
      }
      it = ranges_.erase(it);
      if (range_begin < begin) {
        ranges_.emplace(range_begin, begin);
      }
      if (range_end > end) {
        ranges_.emplace(end, range_end);
      }
    }
  }

 private:
  // Whether no mapped range overlaps [begin, begin + length).
  bool IsFree(uintptr_t begin, size_t length) const {
    auto next = ranges_.upper_bound(begin);
    return (next == ranges_.end() || next->first >= begin + length) &&
           (next == ranges_.begin() || std::prev(next)->second <= begin);
  }

  const uintptr_t first_;
  const uintptr_t end_;
  std::map<uintptr_t, uintptr_t> ranges_;  // the mapped ranges, begin to end
};

// Where ReserveHeapWindow put the window: its base, aligned to 4 GiB, where the offset 0 lies, and the reserved range
// [begin, end), which ends 4 GiB above the base. All 0 when nothing was reserved. `guard` says the null page
// [base, base + page) is reserved with no access, so a null decode (address base) faults there and ART's implicit
// null check (NullPointerHandler) catches it (ADR-0053 D3); false means the decode keeps null 0 with a select instead.
struct HeapWindowPlace {
  uintptr_t base;
  uintptr_t begin;
  uintptr_t end;
  bool guard;
};

// How far 32-bit offsets reach from the base.
static constexpr size_t kHeapWindowReach = size_t{4} << 30;

// Reserves the window through `map(hint, length)`, which maps `length` bytes without access and returns their
// address, 0 when the system has no room: at `hint` when that exact range is free and otherwise wherever the system
// has room, as mmap takes an address hint with no MAP_FIXED (Darwin has no MAP_FIXED_NOREPLACE, and MAP_FIXED would
// silently replace whatever is mapped there). `unmap(begin, length)` gives a range back. The window is:
// - all 4 GiB at a base aligned to 4 GiB, cut out of a mapping of 8 GiB, where the system has room for that;
// - else only their upper 2 GiB, the lowest in the largest piece it has, found to `granularity`, the page size.
//   Without increased-memory-limit, iOS gives an app 7.84 GiB in 4 pieces, the largest 6.00 GiB (spike S2,
//   ADR-0012). The upper half fits into any piece of 6 GiB less a page, its offsets all lie above 2^31, and it holds
//   ART's spaces at heapsize 512m (1,600 MiB); all 4 GiB would leave 2 GiB less to the rest of the process.
// In the full-4 GiB window the null page [base, base + page) lies inside the one mapping and is never given out, so
// it guards null on its own. Only the window (and, in the upper-half mode, the guard page beside it) stays mapped;
// HeapWindowPlace is all 0 when nothing fits.
template <typename Map, typename Unmap>
HeapWindowPlace ReserveHeapWindow(Map map, Unmap unmap, size_t granularity) {
  constexpr size_t kHalf = kHeapWindowReach / 2;
  const auto round_up = [](uintptr_t address) {
    return (address + kHeapWindowReach - 1u) & ~uintptr_t{kHeapWindowReach - 1u};
  };
  // Keeps [begin, base + 4 GiB) of the mapping [piece, piece + length) and gives the rest back.
  const auto keep = [&](uintptr_t piece, size_t length, uintptr_t base, uintptr_t begin) {
    const uintptr_t end = base + kHeapWindowReach;
    if (begin != piece) {
      unmap(piece, begin - piece);
    }
    if (end != piece + length) {
      unmap(end, piece + length - end);
    }
    return HeapWindowPlace{base, begin, end, false};
  };
  if (const uintptr_t piece = map(0u, 2 * kHeapWindowReach); piece != 0u) {
    HeapWindowPlace place = keep(piece, 2 * kHeapWindowReach, round_up(piece), round_up(piece));
    place.guard = true;  // the window's first page is inside [base, base + 4 GiB) and never given out
    return place;
  }
  size_t largest = 0u;
  for (size_t too_large = 2 * kHeapWindowReach; too_large - largest > granularity;) {
    const size_t length = (largest + (too_large - largest) / 2) / granularity * granularity;
    if (length <= largest) {
      break;
    }
    if (const uintptr_t probe = map(0u, length); probe != 0u) {
      unmap(probe, length);
      largest = length;
    } else {
      too_large = length;
    }
  }
  const uintptr_t piece = largest >= kHalf ? map(0u, largest) : 0u;
  if (piece == 0u) {
    return HeapWindowPlace{0u, 0u, 0u, false};
  }
  // Try aligned bases from the lowest whose upper half begins in the piece, upward: the first whose null page can be
  // a no-access guard wins (ADR-0053 D3, decided 2026-10-27). The lowest base's guard may lie below the piece — hint
  // at it with no MAP_FIXED and keep it only on an exact hit (Darwin has no MAP_FIXED_NOREPLACE); every higher base
  // lies at or above the piece, so its guard is inside the piece and always securable. Bounded, so a crafted address
  // space cannot spin. If no base is guardable the window is given back and nothing is reserved, so the caller
  // refuses rather than run the unconditional decode with no guard.
  constexpr int kMaxBaseTries = 16;
  const uintptr_t lowest = piece > kHalf ? round_up(piece - kHalf) : kHeapWindowReach;
  int tries = 0;
  for (uintptr_t base = lowest; base + kHeapWindowReach <= piece + largest && tries < kMaxBaseTries;
       base += kHeapWindowReach, ++tries) {
    if (base < piece) {
      // The guard lies below the piece: reserve it by a hint at base; on anything but an exact hit, give it back and
      // try the next base.
      const uintptr_t guard = map(base, granularity);
      if (guard != base) {
        if (guard != 0u) unmap(guard, granularity);
        continue;
      }
    }
    // Committed. Keep the window [begin, end) and, where the guard lies inside the piece, the guard page
    // [base, base + granularity); give the rest of the piece back.
    const uintptr_t begin = base + kHalf, end = base + kHeapWindowReach;
    if (base > piece) unmap(piece, base - piece);
    const uintptr_t after_guard = base >= piece ? base + granularity : piece;
    if (after_guard < begin) unmap(after_guard, begin - after_guard);
    if (end < piece + largest) unmap(end, piece + largest - end);
    return HeapWindowPlace{base, begin, end, true};
  }
  unmap(piece, largest);
  return HeapWindowPlace{0u, 0u, 0u, false};
}

}  // namespace art

#endif  // ART_LIBARTBASE_BASE_HEAP_WINDOW_RANGES_H_
