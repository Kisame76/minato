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

}  // namespace art

#endif  // ART_LIBARTBASE_BASE_HEAP_WINDOW_RANGES_H_
