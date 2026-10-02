// Checks where the heap window places MemMap's low_4gb mappings (ADR-0033 decision 3), with ART's own header for it,
// runtime/art/overlay/art/libartbase/base/heap_window_ranges.h: the window fills from the top, so that objects have
// offsets above 2^31 from the start; its last page is never given out, as its first is not, so that no mapping ends
// at 2^32, whose offset would be 0, which stands for null; a hint is taken where the window has room for the
// mapping there; and what goes back is given out again from the top.
#include "heap_window_ranges.h"

#include "check.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>

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

}  // namespace

int main() {
    return apk::testing::run_all();
}
