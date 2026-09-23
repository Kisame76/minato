// Checks the scanner and the stubs of the rewriter (ADR-0010, ADR-0019) against code that the host
// assembler and linker produced (rewrite_reference.S): every emitted word, including branch and
// page offsets, must match what the real toolchain writes for the same instructions.
#include "apkconv/rewrite.h"

#include "check.h"

#include <cstring>
#include <vector>

extern "C" {
extern const std::uint32_t ref_thread_pointer_read_x5[];
extern const std::uint32_t ref_thread_pointer_write_x7[];
extern const std::uint32_t ref_thread_pointer_write_x16[];
extern const std::uint32_t ref_thread_pointer_write_xzr[];
extern const std::uint32_t ref_syscall_veneer[];
extern const std::uint32_t ref_site[];
extern const std::uint32_t ref_after_site[];
extern const std::uint32_t ref_patterns[];
extern const std::uint32_t ref_patterns_end[];
extern const std::uint32_t ref_x18[];
extern const std::uint32_t ref_x18_end[];
extern std::uint64_t ref_syscall_slot;
}

namespace {

using apk::rewrite::Site;
using apk::rewrite::SiteKind;

std::uint64_t address_of(const void *pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer);
}

std::uint32_t word(const std::vector<std::byte> &bytes, std::size_t index) {
    std::uint32_t value;
    std::memcpy(&value, bytes.data() + 4 * index, sizeof value);
    return value;
}

/// Emits the stub of `site` as if the site sat at `site_address` and the stub at `stub_address`.
std::pair<std::uint32_t, std::vector<std::byte>> emit_at(const Site &site, std::uint64_t site_address,
                                                         std::uint64_t stub_address) {
    std::vector<std::byte> stub(apk::rewrite::stub_size(site));
    const auto patch =
        apk::rewrite::emit(site, site_address, stub_address, address_of(&ref_syscall_slot), stub);
    return {patch, stub};
}

TEST(scan_finds_the_three_patterns_and_nothing_else) {
    const auto size = address_of(ref_patterns_end) - address_of(ref_patterns);
    const std::span code(reinterpret_cast<const std::byte *>(ref_patterns), size);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x1000, findings);

    const std::vector<Site> expected = {
        {0x1000, SiteKind::thread_pointer_read, 3},
        {0x1004, SiteKind::thread_pointer_write, 9},
        {0x1008, SiteKind::syscall, 0},
        {0x100c, SiteKind::thread_pointer_read, 30},
        {0x1010, SiteKind::thread_pointer_write, 31},
    };
    CHECK(findings.sites == expected);
    CHECK(findings.other_syscalls == std::vector<std::uint64_t>{0x1014});
}

TEST(scan_sorts_x18_into_reads_writes_and_register_dumps) {
    const auto size = address_of(ref_x18_end) - address_of(ref_x18);
    const std::span code(reinterpret_cast<const std::byte *>(ref_x18), size);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x2000, findings);

    CHECK(findings.x18_writes == (std::vector<std::uint64_t>{0x2000, 0x2004}));
    CHECK(findings.x18_reads == (std::vector<std::uint64_t>{0x2008, 0x200c, 0x2028, 0x2034}));
    CHECK(findings.x18_saves == (std::vector<std::uint64_t>{0x2014, 0x2020}));
    CHECK(findings.sites.empty());
}

TEST(a_register_dump_at_the_edge_of_the_code_is_a_read) {
    // stp x18, x19, [x0, #0x90] as the first and as the last word: one neighbour is missing.
    const std::uint32_t words[] = {0xa909'4c12, 0xa90a'5414, 0xa908'4410, 0xa909'4c12};
    const std::span code(reinterpret_cast<const std::byte *>(words), sizeof words);
    apk::rewrite::Findings findings;
    apk::rewrite::scan(code, 0x3000, findings);
    CHECK(findings.x18_reads == (std::vector<std::uint64_t>{0x3000, 0x300c}));
    CHECK(findings.x18_saves.empty());
}

TEST(scan_rejects_code_that_is_not_word_aligned) {
    const std::vector<std::byte> code(8);
    apk::rewrite::Findings findings;
    CHECK_THROWS(apk::rewrite::RewriteError, apk::rewrite::scan(code, 0x1002, findings), "aligned");
    CHECK_THROWS(apk::rewrite::RewriteError, apk::rewrite::scan(std::span(code).first(6), 0x1000, findings),
                 "aligned");
}

// The final branch of an island goes back to the instruction after the site. Placing the island
// so that this branch lands on the `b _ref_after_site` of the reference veneer (its 6th word) makes
// the linker's branch the expected value.
TEST(the_thread_pointer_read_island_matches_the_assembler) {
    const Site site{0, SiteKind::thread_pointer_read, 5};
    const auto stub_address = address_of(ref_syscall_veneer) + 5 * 4 - 3 * 4;
    const auto [patch, stub] = emit_at(site, address_of(ref_site), stub_address);
    CHECK(stub.size() == 16);
    for (std::size_t index = 0; index < 3; ++index) CHECK(word(stub, index) == ref_thread_pointer_read_x5[index]);
    CHECK(word(stub, 3) == ref_syscall_veneer[5]);  // b _ref_after_site
}

TEST(a_thread_pointer_read_into_xzr_becomes_a_nop) {
    const Site site{0, SiteKind::thread_pointer_read, 31};
    CHECK(apk::rewrite::stub_size(site) == 0);
    const auto [patch, stub] = emit_at(site, 0x4000, 0x1000);
    CHECK(patch == 0xd503'201f);
}

TEST(the_thread_pointer_write_islands_match_the_assembler) {
    const struct {
        std::uint8_t reg;
        const std::uint32_t *reference;
    } cases[] = {{7, ref_thread_pointer_write_x7},
                 {16, ref_thread_pointer_write_x16},
                 {31, ref_thread_pointer_write_xzr}};
    for (const auto &test : cases) {
        const Site site{0, SiteKind::thread_pointer_write, test.reg};
        const auto [patch, stub] = emit_at(site, address_of(ref_site), address_of(ref_syscall_veneer));
        CHECK(stub.size() == 24);
        for (std::size_t index = 0; index < 5; ++index) CHECK(word(stub, index) == test.reference[index]);
        CHECK(word(stub, 5) == ref_syscall_veneer[5]);
    }
}

TEST(the_syscall_veneer_and_the_patched_site_match_the_linker) {
    const Site site{0, SiteKind::syscall, 0};
    const auto [patch, stub] = emit_at(site, address_of(ref_site), address_of(ref_syscall_veneer));
    CHECK(stub.size() == 24);
    for (std::size_t index = 0; index < 6; ++index) CHECK(word(stub, index) == ref_syscall_veneer[index]);
    CHECK(patch == ref_site[0]);  // b _ref_syscall_veneer
}

TEST(stubs_out_of_branch_range_are_rejected) {
    const Site site{0, SiteKind::syscall, 0};
    std::vector<std::byte> stub(apk::rewrite::stub_size(site));
    constexpr std::uint64_t far = 129ull << 20;  // b reaches -128 MiB exactly, so go one further
    CHECK_THROWS(apk::rewrite::RewriteError, apk::rewrite::emit(site, 0x1000 + far, 0x1000, 0x10'0000, stub),
                 "128 MiB");
    CHECK_THROWS(apk::rewrite::RewriteError,
                 apk::rewrite::emit(site, 0x2000, 0x1000, 0x1000 + (5ull << 30), stub), "4 GiB");
    CHECK_THROWS(apk::rewrite::RewriteError, apk::rewrite::emit(site, 0x2000, 0x1000, 0x10'0004, stub),
                 "8-byte");
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
