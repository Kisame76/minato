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
// [begin, end), which ends 4 GiB above the base. All 0 when nothing was reserved.
struct HeapWindowPlace {
  uintptr_t base;
  uintptr_t begin;
  uintptr_t end;
};

// How far 32-bit offsets reach from the base.
static constexpr size_t kHeapWindowReach = size_t{4} << 30;

// Reserves the window through `map(length)`, which maps `length` bytes without access wherever the system has room
// and returns their address, 0 when it has none, and `unmap(begin, length)`:
// - all 4 GiB at a base aligned to 4 GiB, cut out of a mapping of 8 GiB, where the system has room for that;
// - else only their upper 2 GiB, the lowest in the largest piece it has, found to `granularity`, the page size.
//   Without increased-memory-limit, iOS gives an app 7.84 GiB in 4 pieces, the largest 6.00 GiB (spike S2,
//   ADR-0012). The upper half fits into any piece of 6 GiB less a page, its offsets all lie above 2^31, and it holds
//   ART's spaces at heapsize 512m (1,600 MiB); all 4 GiB would leave 2 GiB less to the rest of the process.
// Only the window stays mapped; HeapWindowPlace is all 0 when nothing fits.
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
    return HeapWindowPlace{base, begin, end};
  };
  if (const uintptr_t piece = map(2 * kHeapWindowReach); piece != 0u) {
    return keep(piece, 2 * kHeapWindowReach, round_up(piece), round_up(piece));
  }
  size_t largest = 0u;
  for (size_t too_large = 2 * kHeapWindowReach; too_large - largest > granularity;) {
    const size_t length = (largest + (too_large - largest) / 2) / granularity * granularity;
    if (length <= largest) {
      break;
    }
    if (const uintptr_t probe = map(length); probe != 0u) {
      unmap(probe, length);
      largest = length;
    } else {
      too_large = length;
    }
  }
  const uintptr_t piece = largest >= kHalf ? map(largest) : 0u;
  if (piece == 0u) {
    return HeapWindowPlace{0u, 0u, 0u};
  }
  // The lowest aligned base above 0 whose upper half begins in the piece.
  const uintptr_t base = piece > kHalf ? round_up(piece - kHalf) : kHeapWindowReach;
  if (base + kHeapWindowReach <= piece + largest) {
    return keep(piece, largest, base, base + kHalf);
  }
  unmap(piece, largest);
  return HeapWindowPlace{0u, 0u, 0u};
}

}  // namespace art

#endif  // ART_LIBARTBASE_BASE_HEAP_WINDOW_RANGES_H_
