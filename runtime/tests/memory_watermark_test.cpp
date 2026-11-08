// Checks the memory watermark (memory_watermark.h): the fixed-size block format() writes, that write_now overwrites
// the one slot (so the last reading survives an abrupt end) and keeps a memory-pressure warning once it has come, and
// that sample() reads the process's footprint. The 1 Hz timer and the pressure source (report_to) are not exercised
// here -- they are dispatch, device-checked; this is the writer under them.
#include "memory_watermark.h"

#include "check.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

using namespace apkrt;

/// A temporary file, removed at the end of its scope; write_now pwrites to it, so it must be a seekable regular file.
struct TempFile {
    std::string path;
    int fd = -1;
    TempFile() {
        char name[] = "/tmp/minato-memory-XXXXXX";
        fd = ::mkstemp(name);
        path = name;
    }
    ~TempFile() {
        if (fd >= 0) ::close(fd);
        ::unlink(path.c_str());
    }
    std::string contents() const {
        std::string text;
        char buffer[256];
        ssize_t count = 0;
        for (off_t at = 0; (count = ::pread(fd, buffer, sizeof buffer, at)) > 0; at += count) {
            text.append(buffer, static_cast<std::size_t>(count));
        }
        return text;
    }
    off_t size() const {
        struct stat info{};
        return ::fstat(fd, &info) == 0 ? info.st_size : -1;
    }
};

/// The value of the "<name> " line in `block`, trimmed of its padding spaces; empty when the line is missing.
std::string field(const std::string &block, std::string_view name) {
    const std::string key = std::string(name) + " ";
    std::size_t at = block.find(key);
    while (at != std::string::npos && at != 0 && block[at - 1] != '\n') at = block.find(key, at + 1);
    if (at == std::string::npos) return {};
    const std::size_t start = at + key.size();
    const std::size_t end = block.find('\n', start);
    std::string value = block.substr(start, end - start);
    const std::size_t first = value.find_first_not_of(' ');
    return first == std::string::npos ? std::string() : value.substr(first);
}

/// The length of the whole line "<name> ..." including its newline.
std::size_t line_length(const std::string &block, std::string_view name) {
    const std::size_t at = block.find(std::string(name) + " ");
    if (at == std::string::npos) return 0;
    return block.find('\n', at) - at + 1;
}

TEST(format_writes_a_fixed_size_block_of_parseable_fields) {
    const memory::Watermark watermark{.footprint = 1000, .available = 2000, .peak = 3000, .warning = true};
    char out[memory::block_size + 16];
    const std::size_t length = memory::format(watermark, out, sizeof out);
    CHECK(length == memory::block_size);
    const std::string block(out, length);
    CHECK(field(block, "footprint") == "1000");
    CHECK(field(block, "available") == "2000");
    CHECK(field(block, "peak") == "3000");
    CHECK(field(block, "warning") == "1");
    // Fixed width per line, so the block's size never changes between writes (the pwrite slot).
    CHECK(line_length(block, "footprint") == 31);
    CHECK(line_length(block, "available") == 31);
    CHECK(line_length(block, "peak") == 26);
    CHECK(line_length(block, "warning") == 10);
}

TEST(format_keeps_the_block_size_for_the_largest_values) {
    const memory::Watermark watermark{.footprint = UINT64_MAX, .available = UINT64_MAX, .peak = UINT64_MAX,
                                      .warning = false};
    char out[memory::block_size + 16];
    const std::size_t length = memory::format(watermark, out, sizeof out);
    CHECK(length == memory::block_size);
    const std::string block(out, length);
    CHECK(field(block, "footprint") == "18446744073709551615");
    CHECK(field(block, "warning") == "0");
}

TEST(format_writes_nothing_into_too_small_a_buffer) {
    const memory::Watermark watermark{};
    char out[memory::block_size - 1];
    CHECK(memory::format(watermark, out, sizeof out) == 0);
}

TEST(write_now_fills_the_slot_with_the_process_footprint) {
    const TempFile file;
    memory::write_now(file.fd, false);
    CHECK(file.size() == static_cast<off_t>(memory::block_size));
    const std::string block = file.contents();
    CHECK(!field(block, "footprint").empty());
    CHECK(std::stoull(field(block, "footprint")) > 0);  // the test process holds some memory
    CHECK(!field(block, "peak").empty());
    CHECK(field(block, "warning") == "0");
}

TEST(write_now_overwrites_the_one_slot_each_time) {
    const TempFile file;
    memory::write_now(file.fd, false);
    memory::write_now(file.fd, false);
    CHECK(file.size() == static_cast<off_t>(memory::block_size));  // overwritten at offset 0, not appended
}

TEST(write_now_keeps_a_warning_once_it_has_come) {
    const TempFile file;
    memory::write_now(file.fd, true);
    CHECK(field(file.contents(), "warning") == "1");
    memory::write_now(file.fd, false);  // a later write without a warning does not clear it
    CHECK(field(file.contents(), "warning") == "1");
}

TEST(write_now_does_nothing_for_a_descriptor_below_zero) {
    memory::write_now(-1, false);  // must not crash or fault
}

TEST(sample_reads_the_footprint_and_a_peak_at_least_as_large) {
    const memory::Watermark watermark = memory::sample();
    CHECK(watermark.footprint > 0);
    CHECK(watermark.peak >= watermark.footprint);
}

TEST(format_instrument_writes_the_memory_fields_in_mib) {
    constexpr std::uint64_t mib = std::uint64_t{1} << 20;
    const memory::Instrument reading{.footprint = 6000 * mib, .internal = 5000 * mib, .compressed = 100 * mib,
                                     .external = 200 * mib, .graphics = 300 * mib, .media = 50 * mib,
                                     .purgeable = 40 * mib, .purgeable_compressed = 10 * mib, .other = 500 * mib,
                                     .anon = 5500 * mib, .file = 250 * mib};
    char out[256];
    const std::size_t length = memory::format_instrument(reading, true, out, sizeof out);
    CHECK(std::string(out, length) ==
          "I/apkrt   : mem foot=6000 int=5000 comp=100 gfx=300 media=50 purg=40 purgc=10 other=500 ext=200 "
          "anon=5500 file=250 warn\n");
}

TEST(format_instrument_omits_the_marker_without_a_warning) {
    char out[256];
    const std::size_t length = memory::format_instrument(memory::Instrument{}, false, out, sizeof out);
    CHECK(std::string(out, length) ==
          "I/apkrt   : mem foot=0 int=0 comp=0 gfx=0 media=0 purg=0 purgc=0 other=0 ext=0 anon=0 file=0\n");
}

TEST(sample_instrument_reads_the_footprint_and_some_anonymous_memory) {
    const memory::Instrument reading = memory::sample_instrument();
    CHECK(reading.footprint > 0);
    CHECK(reading.anon > 0);  // the test process holds anonymous mappings
    // The footprint breakdown sums to the footprint: "other" is the remainder for the parts the SDK does not name.
    const std::uint64_t parts = reading.internal + reading.compressed + reading.graphics + reading.media +
                                reading.purgeable + reading.purgeable_compressed;
    if (reading.footprint >= parts) {
        CHECK(parts + reading.other == reading.footprint);
    } else {
        CHECK(reading.other == 0);  // nothing to add when the named parts already exceed the footprint
    }
}

/// A stand-in for libGLESv2's ANGLEMetalMemoryLine, so the test needs no real ANGLE: writes a fixed stats string.
int fake_angle_line(char *line, std::size_t size) {
    static const char stats[] = "gl tex=5/2 priv=1 shared=1 comp=1/0 color=2/1 depth=1/0 msaa=1/1 mless=0 "
                                "buf=2/2 bufpriv=0 pool=0/0 mtl=4";
    const std::size_t length = sizeof stats - 1;
    if (length >= size) return 0;
    std::memcpy(line, stats, length + 1);
    return static_cast<int>(length);
}

memory::GraphicsLineFn present_lookup() { return fake_angle_line; }
memory::GraphicsLineFn absent_lookup() { return nullptr; }

int g_graphics_calls = 0;  // for flaky_lookup: nullptr first, the symbol second, nullptr after
memory::GraphicsLineFn flaky_lookup() { return g_graphics_calls++ == 1 ? fake_angle_line : nullptr; }

TEST(graphics_line_wraps_the_angle_stats_when_the_symbol_is_present) {
    memory::set_graphics_lookup_for_test(present_lookup);
    char out[256];
    const std::size_t length = memory::graphics_line(true, out, sizeof out);
    CHECK(std::string(out, length) ==
          "I/apkrt   : gl tex=5/2 priv=1 shared=1 comp=1/0 color=2/1 depth=1/0 msaa=1/1 mless=0 buf=2/2 "
          "bufpriv=0 pool=0/0 mtl=4 warn\n");
    memory::set_graphics_lookup_for_test(nullptr);  // restore the real lookup, clear the cache
}

TEST(graphics_line_is_empty_without_the_symbol) {
    memory::set_graphics_lookup_for_test(absent_lookup);  // mimic a build where the symbol never resolves
    char out[256];
    CHECK(memory::graphics_line(false, out, sizeof out) == 0);
    memory::set_graphics_lookup_for_test(nullptr);
}

TEST(graphics_line_retries_until_the_symbol_appears_then_caches_it) {
    g_graphics_calls = 0;
    memory::set_graphics_lookup_for_test(flaky_lookup);
    char out[256];
    CHECK(memory::graphics_line(false, out, sizeof out) == 0);  // call 0: lookup misses (ANGLE not loaded) -> no line
    CHECK(memory::graphics_line(false, out, sizeof out) > 0);   // call 1: symbol appears -> line, pointer cached
    CHECK(memory::graphics_line(false, out, sizeof out) > 0);   // call 2: lookup would miss, but the cache answers
    memory::set_graphics_lookup_for_test(nullptr);
}

}  // namespace

int main() { return apk::testing::run_all(); }
