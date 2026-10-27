// Checks where the heap window places MemMap's low_4gb mappings (ADR-0033 decision 3), with ART's own header for it,
// runtime/art/overlay/art/libartbase/base/heap_window_ranges.h: the window fills from the top, so that objects have
// offsets above 2^31 from the start; its last page is never given out, as its first is not, so that no mapping ends
// at 2^32, whose offset would be 0, which stands for null; a hint is taken where the window has room for the
// mapping there; and what goes back is given out again from the top. Also where the window is reserved: all 4 GiB
// where the system has room for 8, else, as without increased-memory-limit on iOS (spike S2: 7.84 GiB in 4 pieces,
// the largest 6.00 GiB), only the upper 2 GiB of an aligned 4, the lowest in the largest piece, found to the page.
#include "heap_window_ranges.h"

#include "check.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <map>
#include <utility>

namespace {

constexpr std::uintptr_t base = std::uintptr_t{1} << 36;  // aligned to 4 GiB, as the reservation's base is
constexpr std::size_t window = std::size_t{4} << 30, page = 16 * 1024, mib = 1 << 20;
constexpr std::uintptr_t top = base + window - page;  // the end of what mappings may use: the last page's start

TEST(the_first_mapping_ends_at_the_reserved_last_page) {
    art::HeapWindowRanges ranges(base, window, page);
    CHECK(ranges.First() == base + page && ranges.End() == top);
    const auto placed = ranges.Place(0, 64 * mib);
    CHECK(placed == top - 64 * mib);
    // Its end as a 32-bit offset is the last page's, never 0.
    CHECK(static_cast<std::uint32_t>(placed + 64 * mib) == 0xffffc000u);
}

TEST(mappings_fill_the_window_from_the_top) {
    art::HeapWindowRanges ranges(base, window, page);
    std::uintptr_t below = top;
    for (const std::size_t length : {64 * mib, 256 * mib, 256 * mib, 16 * page}) {
        const auto placed = ranges.Place(0, length);
        CHECK(placed == below - length);
        ranges.Add(placed, length);
        below = placed;
    }
    CHECK(below - base >= std::uintptr_t{1} << 31);
}

TEST(a_mapping_reaches_right_up_to_the_reserved_page_and_no_further) {
    art::HeapWindowRanges ranges(base, window, page);
    // All of the window but its first and its last page.
    CHECK(ranges.Place(0, window - 2 * page) == base + page);
    CHECK(ranges.Place(0, window - page) == 0);
    // A fixed mapping, into a range of the caller's own: up to the last page, not into it.
    CHECK(ranges.Holds(top - page, page));
    CHECK(!ranges.Holds(top - page, 2 * page));
    CHECK(!ranges.Holds(top, page));
    CHECK(!ranges.Holds(base, page));
}

TEST(a_hint_is_taken_where_the_window_has_room_for_the_mapping) {
    art::HeapWindowRanges ranges(base, window, page);
    const std::uintptr_t hint = base + (std::size_t{1} << 30);
    CHECK(ranges.Place(hint, mib) == hint);
    ranges.Add(hint, mib);
    // A hint taken already, one that reaches into the last page, and ART's preferred begin on Android, which lies
    // outside the window: the top instead.
    CHECK(ranges.Place(hint, mib) == top - mib);
    CHECK(ranges.Place(top - mib / 2, mib) == top - mib);
    CHECK(ranges.Place(0x12c00000u, mib) == top - mib);
}

TEST(what_goes_back_is_given_out_again_from_the_top) {
    art::HeapWindowRanges ranges(base, window, page);
    const auto upper = ranges.Place(0, 8 * mib);
    ranges.Add(upper, 8 * mib);
    const auto lower = ranges.Place(0, 8 * mib);
    ranges.Add(lower, 8 * mib);
    CHECK(upper == top - 8 * mib && lower == upper - 8 * mib);
    // The upper half of the upper mapping goes back: a small mapping takes it, a bigger one goes below the lower.
    ranges.Remove(upper + 4 * mib, 4 * mib);
    CHECK(ranges.Place(0, 4 * mib) == upper + 4 * mib);
    CHECK(ranges.Place(0, 5 * mib) == lower - 5 * mib);
    // All of it back: the top again.
    ranges.Remove(upper, 4 * mib);
    ranges.Remove(lower, 8 * mib);
    CHECK(ranges.Place(0, 16 * mib) == top - 16 * mib);
}

constexpr std::uintptr_t gib = std::uintptr_t{1} << 30;

// An address space as the system keeps it: free pieces; a mapping goes to the lowest piece that holds it, and what is
// unmapped is free again.
class AddressSpace {
public:
    using Ranges = std::map<std::uintptr_t, std::uintptr_t>;  // begin to end

    AddressSpace(std::initializer_list<std::pair<std::uintptr_t, std::uintptr_t>> pieces) {
        for (const auto &[begin, length] : pieces) add(free_, begin, begin + length);
    }

    // mmap with an address hint and no MAP_FIXED: the hint when [hint, hint + length) holds no mapping, else the
    // lowest piece that holds `length` (the kernel ignores a hint it cannot honour). 0 when nothing holds it.
    std::uintptr_t map(std::uintptr_t hint, std::size_t length) {
        if (hint != 0 && is_free(hint, length)) {
            cut(free_, hint, hint + length);
            add(mapped, hint, hint + length);
            return hint;
        }
        for (const auto &piece : free_) {
            if (piece.second - piece.first >= length) {
                const std::uintptr_t begin = piece.first;
                cut(free_, begin, begin + length);
                add(mapped, begin, begin + length);
                return begin;
            }
        }
        return 0;
    }

    void unmap(std::uintptr_t begin, std::size_t length) {
        cut(mapped, begin, begin + length);
        add(free_, begin, begin + length);
    }

    Ranges mapped;

private:
    static void add(Ranges &ranges, std::uintptr_t begin, std::uintptr_t end) {
        const auto next = ranges.lower_bound(begin);
        if (next != ranges.begin() && std::prev(next)->second == begin) {
            begin = std::prev(next)->first;
            ranges.erase(std::prev(next));
        }
        if (const auto after = ranges.find(end); after != ranges.end()) {
            end = after->second;
            ranges.erase(after);
        }
        ranges[begin] = end;
    }

    static void cut(Ranges &ranges, std::uintptr_t begin, std::uintptr_t end) {
        auto it = ranges.upper_bound(begin);
        if (it != ranges.begin()) --it;
        while (it != ranges.end() && it->first < end) {
            const auto [range_begin, range_end] = *it;
            if (range_end <= begin) {
                ++it;
                continue;
            }
            it = ranges.erase(it);
            if (range_begin < begin) ranges[range_begin] = begin;
            if (range_end > end) ranges[end] = range_end;
        }
    }

    // Whether [begin, begin + length) overlaps no mapping, so an mmap hint there would be honoured.
    bool is_free(std::uintptr_t begin, std::size_t length) const {
        auto it = mapped.upper_bound(begin);
        if (it != mapped.begin() && std::prev(it)->second > begin) return false;
        if (it != mapped.end() && it->first < begin + length) return false;
        return true;
    }

    Ranges free_;
};

art::HeapWindowPlace reserve(AddressSpace &space) {
    return art::ReserveHeapWindow(
        [&](std::uintptr_t hint, std::size_t length) { return space.map(hint, length); },
        [&](std::uintptr_t begin, std::size_t length) { space.unmap(begin, length); }, page);
}

// Whether only [begin, end) is mapped, so every probe went back.
bool only_mapped(const AddressSpace &space, std::uintptr_t begin, std::uintptr_t end) {
    return space.mapped.size() == 1 && space.mapped.begin()->first == begin && space.mapped.begin()->second == end;
}

// Whether the window is the upper half of the 4 GiB at `aligned`, with the null page [aligned, aligned + page) kept
// as a no-access guard beside it and nothing else mapped.
bool upper_half_at(const AddressSpace &space, const art::HeapWindowPlace &place, std::uintptr_t aligned) {
    return place.base == aligned && place.begin == aligned + 2 * gib && place.end == aligned + 4 * gib &&
           place.guard && space.mapped.size() == 2 && space.mapped.count(aligned) == 1 &&
           space.mapped.at(aligned) == aligned + page && space.mapped.count(aligned + 2 * gib) == 1 &&
           space.mapped.at(aligned + 2 * gib) == aligned + 4 * gib;
}

TEST(with_room_for_8_gib_the_window_is_all_4_gib_cut_out_of_them) {
    // As with increased-memory-limit (S2: 64 GiB in one piece), which here begins below an aligned address.
    AddressSpace space({{9 * gib / 2, 64 * gib}});
    const auto place = reserve(space);
    CHECK(place.base == 8 * gib && place.begin == 8 * gib && place.end == 12 * gib);
    CHECK(only_mapped(space, 8 * gib, 12 * gib));
    // The null page [base, base + page) is inside that one mapping and never given out, so it guards null.
    CHECK(place.guard);
}

TEST(without_room_for_8_gib_the_window_is_the_upper_2_gib_of_an_aligned_4_in_the_largest_piece) {
    // Shaped like S2 without the entitlement: three small pieces and one of 6 GiB.
    AddressSpace space({{17 * gib / 4, gib / 2}, {5 * gib, gib / 2}, {23 * gib / 4, 832 * mib}, {7 * gib, 6 * gib}});
    const auto place = reserve(space);
    CHECK(upper_half_at(space, place, 8 * gib));
    // Every offset in it lies above 2^31, and the first mapping still ends at the reserved last page.
    art::HeapWindowRanges ranges(place.begin, place.end - place.begin, page);
    CHECK(ranges.First() - place.base > std::uintptr_t{1} << 31);
    CHECK(static_cast<std::uint32_t>(ranges.Place(0, 64 * mib) + 64 * mib) == 0xffffc000u);
}

TEST(the_upper_half_even_where_the_piece_holds_all_4_gib_so_the_rest_keeps_2_gib_more) {
    // [6, 12) GiB holds the aligned [8, 12), but the upper half of [4, 8) comes first: the lowest that fits.
    AddressSpace space({{6 * gib, 6 * gib}});
    CHECK(upper_half_at(space, reserve(space), 4 * gib));
}

TEST(the_upper_half_may_begin_where_the_piece_begins_and_end_where_it_ends) {
    AddressSpace from_its_begin({{6 * gib, 4 * gib}});
    CHECK(upper_half_at(from_its_begin, reserve(from_its_begin), 4 * gib));
    AddressSpace to_its_end({{5 * gib, 3 * gib}});
    CHECK(upper_half_at(to_its_end, reserve(to_its_end), 4 * gib));
    AddressSpace just_the_half({{6 * gib, 2 * gib}});
    CHECK(upper_half_at(just_the_half, reserve(just_the_half), 4 * gib));
}

TEST(the_largest_piece_is_found_to_the_page) {
    // A page above an aligned upper half: the next one ends where the piece ends, which a coarser search misses.
    AddressSpace space({{6 * gib + page, 6 * gib - page}});
    CHECK(upper_half_at(space, reserve(space), 8 * gib));
}

TEST(a_piece_of_6_gib_less_a_page_holds_the_lowest_upper_half_wherever_it_begins) {
    for (std::uintptr_t start = 4 * gib; start < 8 * gib; start += 256 * mib) {
        for (const std::uintptr_t begin : {start, start + page}) {
            AddressSpace space({{begin, 6 * gib - page}});
            // The lowest upper half in the piece begins at its first address 2 GiB above a multiple of 4 GiB.
            const std::uintptr_t half = (begin + 2 * gib + 4 * gib - 1) / (4 * gib) * (4 * gib) - 2 * gib;
            CHECK(upper_half_at(space, reserve(space), half - 2 * gib));
        }
    }
}

TEST(the_null_page_below_the_piece_is_reserved_as_a_guard_in_the_upper_half) {
    // The 6 GiB piece [6, 12) gives the upper half of [4, 8), so the base 4 GiB lies 2 GiB below the piece: the
    // guard page [base, base + page) is reserved there on its own, by a hint, beside the window.
    AddressSpace space({{6 * gib, 6 * gib}});
    const auto place = reserve(space);
    CHECK(upper_half_at(space, place, 4 * gib));
    CHECK(space.mapped.at(4 * gib) == 4 * gib + page);
}

TEST(a_base_with_its_null_page_taken_retries_at_the_next_aligned_base) {
    // The lowest base (4 GiB) has its guard page taken, so the reservation must not clobber it (Darwin MAP_FIXED
    // would) and instead moves up to the next aligned base (8 GiB), whose guard lies inside the piece.
    AddressSpace space({{6 * gib, 6 * gib}});
    space.map(4 * gib, page);  // something else already holds the 4 GiB base's null page
    const auto place = reserve(space);
    CHECK(place.base == 8 * gib && place.begin == 10 * gib && place.end == 12 * gib && place.guard);
    CHECK(space.mapped.count(8 * gib) == 1 && space.mapped.at(8 * gib) == 8 * gib + page);  // the guard
    CHECK(space.mapped.count(10 * gib) == 1 && space.mapped.at(10 * gib) == 12 * gib);      // the window
    CHECK(space.mapped.count(4 * gib) == 1 && space.mapped.at(4 * gib) == 4 * gib + page);  // left intact
    CHECK(space.mapped.size() == 3);
}

TEST(when_no_base_can_be_guarded_nothing_is_reserved_so_the_caller_refuses) {
    // A 4 GiB piece gives only one upper-half base (4 GiB) and no room for 8 GiB. With that base's null page taken,
    // no placement gets a guard, so the reservation yields nothing (D3: never run the unconditional decode with no
    // guard). The piece is given back and the taken page is left intact.
    AddressSpace space({{6 * gib, 4 * gib}});
    space.map(4 * gib, page);
    const auto place = reserve(space);
    CHECK(place.base == 0 && place.begin == 0 && place.end == 0 && !place.guard);
    CHECK(space.mapped.count(4 * gib) == 1 && space.mapped.at(4 * gib) == 4 * gib + page);
    CHECK(space.mapped.size() == 1);  // only the pre-existing mapping; the piece went back
}

TEST(without_either_nothing_is_reserved_and_every_probe_goes_back) {
    // 3 GiB that hold neither an aligned 4 GiB nor the upper half of one.
    AddressSpace space({{17 * gib / 4, gib / 2}, {13 * gib / 2, 3 * gib}});
    const auto place = reserve(space);
    CHECK(place.base == 0 && place.begin == 0 && place.end == 0);
    CHECK(space.mapped.empty());
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
