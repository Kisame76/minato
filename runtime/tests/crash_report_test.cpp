// The crash record's allow list (ADR-0032 decision 8): its form, the privacy rule (a library not on the
// list never appears by name), and its matching rule (exact, '_'/'-'/'.' suffix, bare prefix for a few).
#include "crash_report.h"

#include "check.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

using apkrt::crash::kept_soname;
using apkrt::crash::keep_libs_list;
using std::string_view;

TEST(the_allow_list_is_sorted_unique_and_holds_basenames_only) {
    const auto libs = keep_libs_list();
    CHECK(!libs.empty());
    for (std::size_t i = 0; i < libs.size(); ++i) {
        CHECK(!libs[i].empty());
        CHECK(libs[i].substr(0, 3) != "lib");        // basename, no "lib" prefix
        CHECK(libs[i].find(".so") == string_view::npos);  // and no ".so"
        if (i > 0) CHECK(libs[i - 1] < libs[i]);     // sorted by byte value, so unique too
    }
}

TEST(listed_libraries_are_kept_by_exact_name_or_a_suffix_delimiter) {
    for (const string_view name : {"libc.so", "libm.so", "libunity.so", "libil2cpp.so", "libGLESv2.so",
                                   "libgodot_android.so", "libSDL2.so", "libart.so", "libapkrt.so",
                                   "libc++_shared.so", "libmonosgen-2.0.so", "ld-android.so", "linker64"}) {
        CHECK(kept_soname(name));
    }
}

TEST(the_few_prefix_libraries_are_kept_by_a_bare_prefix) {
    for (const string_view name : {"libpython3.11.so", "libicuuc.so", "libicui18n.so", "libv8android.so",
                                   "libfmodstudio.so", "libgdx-freetype.so", "libcocos2dcpp.so",
                                   "libcrashlytics.so"}) {
        CHECK(kept_soname(name));
    }
}

TEST(an_unlisted_library_is_never_kept_so_its_name_stays_private) {
    // Names that look engine-ish but are not on the list, and plain app names: all masked.
    for (const string_view name : {"libMyGame.so", "libsecretengine.so", "libil2cppextra.so",
                                   "libunityplayer.so", "libsqlite3.so", "libSDL2main.so", "libfoo.so",
                                   "lib.so", "", "libb.so"}) {
        CHECK(!kept_soname(name));
    }
}

namespace {
std::string frame(std::uint64_t address, const apkrt::crash::Module &module) {
    char buffer[128];
    std::size_t at = 0;
    apkrt::crash::format_frame(buffer, sizeof buffer, at, address, module);
    return std::string(buffer, at);
}
}  // namespace

TEST(a_listed_library_frame_shows_its_name_and_offset) {
    CHECK(frame(0x1234, {.found = true, .base = 0x1000, .soname = "libil2cpp.so"}) == "libil2cpp.so+0x234");
    CHECK(frame(0x4008, {.found = true, .base = 0x4000, .soname = "libc.so"}) == "libc.so+0x8");
    CHECK(frame(0x5000, {.found = true, .base = 0x5000, .soname = "libGLESv2.so"}) == "libGLESv2.so+0x0");
}

TEST(an_unlisted_library_frame_is_app_library_n_and_never_shows_the_name) {
    const std::string line = frame(0x20ff, {.found = true, .base = 0x2000, .soname = "libMyGame.so", .app_index = 3});
    CHECK(line == "app library 3+0xff");
    CHECK(line.find("MyGame") == std::string::npos);  // privacy: the name never leaks
}

TEST(an_address_in_no_known_module_is_a_bare_hex_address) {
    CHECK(frame(0xdeadbeef, {.found = false}) == "0xdeadbeef");
}

namespace {
// A synthetic address->module map: 0x1000.. is a listed library, 0x2000.. an app library, else unknown.
apkrt::crash::Module test_resolve(std::uint64_t address, void *) {
    if (address >= 0x1000 && address < 0x2000) return {.found = true, .base = 0x1000, .soname = "libil2cpp.so"};
    if (address >= 0x2000 && address < 0x3000)
        return {.found = true, .base = 0x2000, .soname = "libMyGame.so", .app_index = 2};
    return {.found = false};
}

std::string backtrace(std::uint64_t fp, int limit) {
    char buffer[256];
    std::size_t at = 0;
    apkrt::crash::write_backtrace(buffer, sizeof buffer, at, fp, limit, test_resolve, nullptr);
    return std::string(buffer, at);
}
}  // namespace

TEST(a_backtrace_walks_the_chain_up_and_masks_each_return_address) {
    std::uint64_t frames[6];
    frames[0] = reinterpret_cast<std::uint64_t>(&frames[2]);  // caller frame, strictly upward
    frames[1] = 0x1100;                                       // return address: listed library
    frames[2] = reinterpret_cast<std::uint64_t>(&frames[4]);
    frames[3] = 0x2100;                                       // return address: app library
    frames[4] = 0;                                            // caller 0 -> the record's return address is 0
    frames[5] = 0;
    CHECK(backtrace(reinterpret_cast<std::uint64_t>(&frames[0]), 8) ==
          "#0 libil2cpp.so+0x100\n#1 app library 2+0x100\n");
}

TEST(a_backtrace_from_an_unreadable_frame_pointer_writes_nothing_and_does_not_fault) {
    CHECK(backtrace(0x8, 8).empty());  // 0x8 is aligned and non-zero but unmapped: vm_read_overwrite fails, no crash
}

TEST(a_backtrace_stops_at_a_chain_that_is_not_strictly_upward) {
    std::uint64_t frames[2];
    frames[0] = 0x8;      // caller frame below any real stack address: the chain ends after one frame
    frames[1] = 0x1100;
    CHECK(backtrace(reinterpret_cast<std::uint64_t>(&frames[0]), 8) == "#0 libil2cpp.so+0x100\n");
}

TEST(a_backtrace_honours_the_frame_limit) {
    std::uint64_t frames[8];
    for (int i = 0; i < 3; ++i) {
        frames[2 * i] = reinterpret_cast<std::uint64_t>(&frames[2 * i + 2]);
        frames[2 * i + 1] = 0x1000 + 0x10 * static_cast<std::uint64_t>(i);
    }
    frames[6] = 0;
    frames[7] = 0;
    const std::string two = backtrace(reinterpret_cast<std::uint64_t>(&frames[0]), 2);
    CHECK(two == "#0 libil2cpp.so+0x0\n#1 libil2cpp.so+0x10\n");  // stops at the limit, not at the chain's end
}

TEST(resolve_names_the_object_and_gives_the_guest_vaddr_as_the_offset) {
    using apkrt::crash::LoadedObject;
    const LoadedObject objects[] = {
        {.image_begin = 0x1000, .image_end = 0x2000, .bias = 0x1000, .soname = "libil2cpp.so"},   // listed
        {.image_begin = 0x3000, .image_end = 0x4000, .bias = 0x2800, .soname = "libMyGame.so"},    // app library 1
        {.image_begin = 0x5000, .image_end = 0x6000, .bias = 0x4800, .soname = "libOther.so"},     // app library 2
    };
    // A listed object: its name and the guest vaddr (host - bias).
    const auto il2cpp = apkrt::crash::resolve_in_objects(0x1500, objects);
    char b1[64]; std::size_t a1 = 0;
    apkrt::crash::format_frame(b1, sizeof b1, a1, 0x1500, il2cpp);
    CHECK(std::string(b1, a1) == "libil2cpp.so+0x500");  // 0x1500 - 0x1000
    // Non-listed objects: "app library N" by load order, never the name.
    const auto game = apkrt::crash::resolve_in_objects(0x3500, objects);
    CHECK(game.found && game.app_index == 1);
    char b2[64]; std::size_t a2 = 0;
    apkrt::crash::format_frame(b2, sizeof b2, a2, 0x3500, game);
    CHECK(std::string(b2, a2) == "app library 1+0xd00");  // 0x3500 - 0x2800
    CHECK(apkrt::crash::resolve_in_objects(0x5500, objects).app_index == 2);
    CHECK(!apkrt::crash::resolve_in_objects(0x9999, objects).found);  // in no object
}

TEST(resolve_registered_matches_resolve_in_objects_but_lock_free) {
    // The signal-frames resolver: the loader appends modules, a signal handler reads them with no lock or allocation.
    apkrt::crash::reset_modules();
    apkrt::crash::register_module(0x1000, 0x2000, 0x1000, "libil2cpp.so");  // listed
    apkrt::crash::register_module(0x3000, 0x4000, 0x2800, "libMyGame.so");   // app library 1
    apkrt::crash::register_module(0x5000, 0x6000, 0x4800, "libOther.so");    // app library 2
    const auto il2cpp = apkrt::crash::resolve_registered(0x1500, nullptr);
    char b1[64]; std::size_t a1 = 0;
    apkrt::crash::format_frame(b1, sizeof b1, a1, 0x1500, il2cpp);
    CHECK(std::string(b1, a1) == "libil2cpp.so+0x500");
    const auto game = apkrt::crash::resolve_registered(0x3500, nullptr);
    CHECK(game.found && game.app_index == 1);
    char b2[64]; std::size_t a2 = 0;
    apkrt::crash::format_frame(b2, sizeof b2, a2, 0x3500, game);
    CHECK(std::string(b2, a2) == "app library 1+0xd00");
    CHECK(apkrt::crash::resolve_registered(0x5500, nullptr).app_index == 2);
    CHECK(!apkrt::crash::resolve_registered(0x9999, nullptr).found);  // in no module
    apkrt::crash::reset_modules();
    CHECK(!apkrt::crash::resolve_registered(0x1500, nullptr).found);  // emptied
}

namespace {
// Captures one write_fault_line() call through a pipe, so the exact bytes it writes can be checked.
std::string fault_line(int signal, int code, std::uint64_t address, std::uint64_t pc) {
    int fds[2];
    if (::pipe(fds) != 0) return "<pipe failed>";
    apkrt::crash::write_fault_line(fds[1], signal, code, address, pc);
    ::close(fds[1]);
    char buffer[512];
    const ssize_t read = ::read(fds[0], buffer, sizeof buffer);
    ::close(fds[0]);
    return read > 0 ? std::string(buffer, static_cast<std::size_t>(read)) : std::string();
}
}  // namespace

TEST(a_fault_line_names_the_signal_the_faulting_library_and_the_fault_address) {
    // The fault diagnostic (ADR-0032 decision 8): when a synchronous fault reaches a guest handler, one line names
    // the signal, the faulting library (masked), the si_code and the fault address. The library comes from the
    // lock-free module registry the loader fills, so it obeys the same privacy mask as the crash record.
    apkrt::crash::reset_modules();
    apkrt::crash::register_module(0x100000, 0x200000, 0x100000, "libil2cpp.so");  // listed engine library
    apkrt::crash::register_module(0x300000, 0x400000, 0x300000, "libMyGame.so");  // app library 1 (masked)
    // A SIGSEGV whose pc is in a listed engine library: named, with the guest vaddr as the offset.
    CHECK(fault_line(11, 1, 0xdead0000, 0x100500) ==
          "E/apkrt: fault SIGSEGV code 1 at libil2cpp.so+0x500, addr 0xdead0000, to the app's handler\n");
    // A pc in an unlisted library never shows the name (privacy): "app library N".
    const std::string masked = fault_line(11, 2, 0x12340000, 0x300040);
    CHECK(masked == "E/apkrt: fault SIGSEGV code 2 at app library 1+0x40, addr 0x12340000, to the app's handler\n");
    CHECK(masked.find("MyGame") == std::string::npos);
    // SIGABRT from abort(): no fault address, a pc in no known module is a bare hex address.
    CHECK(fault_line(6, 0, 0, 0x999) == "E/apkrt: fault SIGABRT code 0 at 0x999, addr 0x0, to the app's handler\n");
    // SIGBUS and a negative si_code (a raised signal, e.g. SI_TKILL) are printed as they are.
    CHECK(fault_line(7, -6, 0, 0x100010) ==
          "E/apkrt: fault SIGBUS code -6 at libil2cpp.so+0x10, addr 0x0, to the app's handler\n");
    apkrt::crash::reset_modules();
}

// MARK: - The fault-site line (a real session's guest SIGSEGV or SIGBUS in a converted library)

namespace {

// Captures one write_fault_site_line() call through a pipe.
std::string site_line(const apkrt::crash::FaultSite &site) {
    int fds[2];
    if (::pipe(fds) != 0) return "<pipe failed>";
    apkrt::crash::write_fault_site_line(fds[1], site);
    ::close(fds[1]);
    char buffer[2048];
    const ssize_t read = ::read(fds[0], buffer, sizeof buffer);
    ::close(fds[0]);
    return read > 0 ? std::string(buffer, static_cast<std::size_t>(read)) : std::string();
}

// The line up to its register list (" regs ..."), which the_line_ends_with_every_register_and_sp checks on its own.
std::string before_registers(const std::string &line) { return line.substr(0, line.find(" regs ")); }

// Two converted libraries as the loader registers them: IL2CPP and the app's own (masked) library.
void register_two() {
    apkrt::crash::reset_modules();
    apkrt::crash::register_module(0x100000, 0x200000, 0x100000, "libil2cpp.so");
    apkrt::crash::register_module(0x300000, 0x400000, 0x300000, "libMyGame.so");
}

apkrt::crash::FaultSite site_at(std::uint64_t pc, std::uint64_t lr, std::uint64_t far, std::uint32_t instruction) {
    apkrt::crash::FaultSite site;
    site.signal = 11;
    site.pc = pc;
    site.lr = lr;
    site.far = far;
    for (std::uint64_t &value : site.registers) value = 0x1'2345'6000;  // addresses, none 0, none the far
    site.instruction_read = true;
    site.instruction = instruction;
    site.rewritten_words = 0;
    return site;
}

}  // namespace

TEST(a_zeroed_x18_as_the_base_shows_in_the_fault_site_line) {
    // ldrb w8, [x18, x9] at libil2cpp +0x4df6c with the hardware x18 0 and x9 0x1087b, while the guest's x18 (TSD slot
    // 11) is 0x7000: the line names the role, both offsets, far, the hardware x18 and the guest's with where it came
    // from, the registers that hold 0 and the far, and the form with its base. This is a missed x18 site: the load
    // used the hardware register, which iOS zeroes, where the guest's x18 held a table.
    register_two();
    auto site = site_at(0x14df6c, 0x14de10, 0x1087b, 0x3869'6A48);
    site.registers[18] = 0;
    site.registers[9] = 0x1087b;
    site.guest_x18 = 0x7000;
    CHECK(before_registers(site_line(site)) ==
          "E/apkrt: fault-site SIGSEGV lib IL2CPP pc +0x4df6c lr +0x4de10 far 0x1087b x18 0x0 gx18 0x7000 slot11 zero "
          "x18 far-in x9 rewritten-words 0 form load byte register-offset base x18=0x0 index x9=0x1087b base-is null "
          "heap-window - va=0 vahole=0");
    apkrt::crash::reset_modules();
}

TEST(the_fault_site_line_never_names_the_library) {
    // The app's own library is "other", its name never appears; an lr in another library names that one's role.
    register_two();
    auto site = site_at(0x300040, 0x100500, 0x8, 0xF940'0420);  // ldr x0, [x1, #8]
    site.registers[1] = 0;
    site.signal = 7;
    site.rewritten_words = 3;
    const std::string line = site_line(site);
    CHECK(before_registers(line) ==
          "E/apkrt: fault-site SIGBUS lib other pc +0x40 lr IL2CPP +0x500 far 0x8 x18 0x123456000 gx18 0x0 slot11 zero "
          "x1 far-in - rewritten-words 3 form load double unsigned-offset base x1=0x0 imm 0x8 base-is null "
          "heap-window - va=0 vahole=0");
    CHECK(line.find("MyGame") == std::string::npos);
    apkrt::crash::reset_modules();
}

TEST(the_fault_site_line_says_when_it_cannot_read_or_decode_and_skips_a_pc_in_no_library) {
    register_two();
    auto unreadable = site_at(0x100010, 0, 0x10, 0);
    unreadable.instruction_read = false;
    unreadable.rewritten_words = -1;
    CHECK(site_line(unreadable).find("lr 0x0 far 0x10 x18 0x123456000 gx18 0x0 slot11 zero - far-in - rewritten-words "
                                     "unknown form unreadable base-is - heap-window - va=0 vahole=0 regs ") !=
          std::string::npos);
    const auto branch = site_at(0x100010, 0, 0x10, 0xD63F'0100);  // blr x8: no load or store
    CHECK(before_registers(site_line(branch)).ends_with(" form none base-is - heap-window - va=0 vahole=0"));
    CHECK(site_line(site_at(0x9999, 0, 0, 0x3869'6A48)).empty());  // pc in no registered library: nothing
    apkrt::crash::reset_modules();
}

TEST(the_guest_x18_of_a_renamed_function_says_rename) {
    // In a (c)-renamed function's range the guest's x18 lives in the rename register, as setup_frame reads it: the line
    // says so, so a reader does not take slot 11's stale value for it.
    register_two();
    auto site = site_at(0x100040, 0, 0x8, 0xF940'0420);  // ldr x0, [x1, #8]
    site.guest_x18 = 0xabc0;
    site.guest_x18_renamed = true;
    CHECK(site_line(site).find(" x18 0x123456000 gx18 0xabc0 rename zero ") != std::string::npos);
    apkrt::crash::reset_modules();
}

TEST(the_form_gives_the_base_and_index_values_the_offset_and_the_address_space) {
    // ldr x0, [x20, #0x878] faulting at 0x10878, as an app's start did: x20's value and the decoded offset tell a NULL
    // object reached through a split offset (x20 = 0x10000) from a link-time address the loader never biased. A
    // register offset gives the index's value and, where it is not a plain 64-bit index, its extend and shift; sp as
    // the base gives sp's value. va= and vahole= (MiB, as on the mem line) close the line: the address space at the
    // fault.
    register_two();
    auto object = site_at(0x300040, 0, 0x10878, 0xF944'3E80);  // ldr x0, [x20, #0x878]
    object.registers[20] = 0x10000;
    object.virtual_size = std::uint64_t{3} << 30;
    object.largest_hole = std::uint64_t{512} << 20;
    CHECK(before_registers(site_line(object))
              .ends_with(" form load double unsigned-offset base x20=0x10000 imm 0x878 base-is none heap-window - "
                         "va=3072 vahole=512"));
    auto indexed = site_at(0x100020, 0, 0x1087b, 0x3869'6A48);  // ldrb w8, [x18, x9]
    indexed.registers[18] = 0;
    indexed.registers[9] = 0x1087b;
    CHECK(before_registers(site_line(indexed))
              .ends_with(" form load byte register-offset base x18=0x0 index x9=0x1087b base-is null heap-window - "
                         "va=0 vahole=0"));
    auto scaled = site_at(0x100020, 0, 0x10, 0xF869'DA80);  // ldr x0, [x20, w9, sxtw #3]
    scaled.registers[9] = 0xffff'fffe;
    CHECK(site_line(scaled).find(" base x20=0x123456000 index x9=0xfffffffe sxtw 3 base-is ") != std::string::npos);
    auto below = site_at(0x100020, 0, 0xff8, 0xF85F'8040);  // ldur x0, [x2, #-8]
    CHECK(site_line(below).find(" unscaled base x2=0x123456000 imm -0x8 base-is ") != std::string::npos);
    auto stack = site_at(0x100020, 0, 0x8, 0xF940'07E0);  // ldr x0, [sp, #8]
    stack.sp = 0x16f000;
    CHECK(site_line(stack).find(" unsigned-offset base sp=0x16f000 imm 0x8 base-is ") != std::string::npos);
    apkrt::crash::reset_modules();
}

TEST(the_line_ends_with_every_register_and_sp) {
    // x0..x30 and sp, values only, so a reader can follow where the base's value came from (the register it was loaded
    // through, a caller's argument); still one write.
    register_two();
    auto site = site_at(0x300040, 0, 0x8, 0xF940'0420);  // ldr x0, [x1, #8]
    for (std::uint64_t number = 0; number < 31; ++number) site.registers[number] = 0x7000'0000'0000 + number * 0x10;
    site.sp = 0x16f'e000;
    std::string want = " regs";
    for (std::uint64_t number = 0; number < 31; ++number) {
        want += std::format(" x{}=0x{:x}", number, 0x7000'0000'0000 + number * 0x10);
    }
    want += " sp=0x16fe000\n";
    const std::string line = site_line(site);
    CHECK(line.ends_with(want));
    CHECK(line.find('\n') == line.size() - 1);  // one line, one write
    apkrt::crash::reset_modules();
}

TEST(the_base_value_is_classified_against_the_small_numbers_minato_hands_out) {
    // The value the faulting access read through, against what Minato gives a guest as a small number where Android
    // gives an address: an ART heap reference (on Android the object's address below 4 GiB, here its offset from the
    // heap window's base, ADR-0033), a thread or process ID, an open file descriptor, and a JNI reference by ART's kind
    // bits. Every class that matches, "none" for none, "null" for 0; "heap-window" names the window's base.
    register_two();
    auto site = site_at(0x300040, 0, 0x10ab8, 0xF942'0680);  // ldr x0, [x20, #0x408]
    site.heap_window_base = 0x2'0000'0000;
    site.heap_window_size = 0x1'0000'0000;
    site.pid = 66000;
    site.last_tid = 67300;
    const auto base_is = [&](std::uint64_t value) {
        site.registers[20] = value;
        const std::string line = before_registers(site_line(site));
        const auto at = line.find(" base-is ");
        return at == std::string::npos ? line : line.substr(at + 9, line.find(' ', at + 9) - at - 9);
    };
    CHECK(base_is(0x106b0) == "heap-ref,tid");  // an object's offset in the window, and among the tids handed out
    CHECK(base_is(0x7'0000'0010) == "none");    // above the window's 4 GiB: an address, no small number
    CHECK(base_is(0x106b4) == "tid");           // not aligned as an object is
    CHECK(base_is(66000) == "heap-ref,pid");
    CHECK(base_is(0) == "null");
    CHECK(base_is(0x2a6) == "jni-global");
    CHECK(base_is(0x2a7) == "jni-weak");
    CHECK(base_is(0x7f00'1234'5679) == "jni-local");
    int fds[2];
    CHECK(::pipe(fds) == 0);
    const int open_fd = ::fcntl(fds[1], F_DUPFD, 200);  // a number whose low bits are no JNI kind
    CHECK(open_fd >= 200 && (open_fd & 3) == 0);
    site.pid = 0;
    site.last_tid = 0;
    site.heap_window_base = 0;
    site.heap_window_size = 0;
    CHECK(base_is(static_cast<std::uint64_t>(open_fd)) == "fd");
    CHECK(before_registers(site_line(site)).ends_with(" heap-window - va=0 vahole=0"));
    site.heap_window_base = 0x2'0000'0000;
    site.heap_window_size = 0x1'0000'0000;
    CHECK(before_registers(site_line(site)).ends_with(" heap-window 0x200000000 va=0 vahole=0"));
    ::close(open_fd);
    ::close(fds[0]);
    ::close(fds[1]);
    apkrt::crash::reset_modules();
}

TEST(the_heap_window_is_read_through_the_variables_the_loader_found) {
    // dl.cpp finds ART's gHeapWindowBase and gHeapWindowSize when the library that defines them loads and hands their
    // addresses over; the fault path reads them with no lock. Before that, or without ART: 0 and 0.
    std::uint64_t base = 0, size = 0;
    apkrt::crash::heap_window(base, size);
    CHECK(base == 0 && size == 0);
    static std::uint64_t art_base = 0x3'0000'0000, art_size = 0x8000'0000;
    apkrt::crash::note_heap_window(&art_base, &art_size);
    apkrt::crash::heap_window(base, size);
    CHECK(base == 0x3'0000'0000 && size == 0x8000'0000);
    apkrt::crash::note_heap_window(nullptr, nullptr);
}

TEST(library_roles_follow_minatos_own_classification) {
    CHECK(std::string(apkrt::crash::library_role("libil2cpp.so")) == "IL2CPP");
    CHECK(std::string(apkrt::crash::library_role("libunity.so")) == "engine");
    CHECK(std::string(apkrt::crash::library_role("libmain.so")) == "engine");
    CHECK(std::string(apkrt::crash::library_role("libmonosgen-2.0.so")) == "engine");
    CHECK(std::string(apkrt::crash::library_role("libc.so")) == "other");     // Android's own: listed, not an engine
    CHECK(std::string(apkrt::crash::library_role("libMyGame.so")) == "other");
    CHECK(std::string(apkrt::crash::library_role("libil2cppx.so")) == "other");  // not il2cpp by a bare prefix
}

TEST(pairs_and_negative_offsets_decode_in_the_form) {
    register_two();
    auto pair = site_at(0x100020, 0, 0x10, 0xA941'0640);  // ldp x0, x1, [x18, #16]
    CHECK(before_registers(site_line(pair))
              .ends_with(" form load pair-double pair-offset base x18=0x123456000 imm 0x10 base-is none heap-window - "
                         "va=0 vahole=0"));
    auto unscaled = site_at(0x100020, 0, 0xff8, 0xF85F'8040);  // ldur x0, [x2, #-8]
    CHECK(before_registers(site_line(unscaled))
              .ends_with(" form load double unscaled base x2=0x123456000 imm -0x8 base-is none heap-window - va=0 "
                         "vahole=0"));
    auto stack = site_at(0x100020, 0, 0x8, 0xF940'07E0);  // ldr x0, [sp, #8]
    CHECK(before_registers(site_line(stack))
              .ends_with(" form load double unsigned-offset base sp=0x0 imm 0x8 base-is null heap-window - va=0 "
                         "vahole=0"));
    apkrt::crash::reset_modules();
}

int main() { return apk::testing::run_all(); }
