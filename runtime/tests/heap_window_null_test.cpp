// Checks the heap window's null-guard predicate (ADR-0053 decision 3), ART's own header for it,
// runtime/art/overlay/art/libartbase/base/heap_window_null.h: a null reference decodes to the window base B, so a
// null dereference faults in the guard page [B, B + page); the handler also keeps recognizing the zero page [0, page)
// that Android and the select-decode fallback fault in, and in both cases hands the NPE path the offset into the
// absent object.
#include "heap_window_null.h"

#include "check.h"

#include <cstddef>
#include <cstdint>

namespace {

constexpr std::uintptr_t base = std::uintptr_t{1} << 36;  // a 4 GiB-aligned window base, as the reservation's is
constexpr std::size_t page = 16 * 1024;                   // iOS's page (fixed decision 7)

TEST(a_fault_in_the_zero_page_is_a_null_check_with_its_address_as_the_offset) {
    std::uintptr_t offset = 123;
    CHECK(art::HeapWindowNullFault(0, base, page, &offset) && offset == 0);
    CHECK(art::HeapWindowNullFault(8, base, page, &offset) && offset == 8);
    CHECK(art::HeapWindowNullFault(page - 1, base, page, &offset) && offset == page - 1);
}

TEST(a_fault_in_the_guard_page_is_a_null_check_with_the_offset_from_the_base) {
    std::uintptr_t offset = 123;
    CHECK(art::HeapWindowNullFault(base, base, page, &offset) && offset == 0);
    CHECK(art::HeapWindowNullFault(base + 8, base, page, &offset) && offset == 8);
    CHECK(art::HeapWindowNullFault(base + page - 1, base, page, &offset) && offset == page - 1);
}

TEST(a_fault_outside_both_pages_is_not_a_null_check) {
    std::uintptr_t offset = 123;
    CHECK(!art::HeapWindowNullFault(page, base, page, &offset));         // just past the zero page
    CHECK(!art::HeapWindowNullFault(base + page, base, page, &offset));  // just past the guard page
    CHECK(!art::HeapWindowNullFault(base - 1, base, page, &offset));     // just below the base
    CHECK(offset == 123);                                               // left untouched on a miss
}

TEST(before_the_window_is_reserved_only_the_zero_page_counts) {
    std::uintptr_t offset = 123;
    CHECK(art::HeapWindowNullFault(8, 0, page, &offset) && offset == 8);  // base 0: the zero page still works
    CHECK(!art::HeapWindowNullFault(page, 0, page, &offset));             // and nothing above it is a null check
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
