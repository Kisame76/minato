// The crash record's allow list (ADR-0032 decision 8): its form, the privacy rule (a library not on the
// list never appears by name), and its matching rule (exact, '_'/'-'/'.' suffix, bare prefix for a few).
#include "crash_report.h"

#include "check.h"

#include <cstdint>
#include <string>
#include <string_view>

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

int main() { return apk::testing::run_all(); }
