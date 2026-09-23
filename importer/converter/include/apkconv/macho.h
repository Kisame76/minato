// Writer for the container dylibs the loader gets (ADR-0008, ADR-0019).
//
// The container is deliberately dull: segments, one section per segment at most, no imports, no
// exports, no chained fixups and no dyld info. Everything the loader needs sits in the metadata
// section, which it finds with getsectiondata() on the loaded image. Less for dyld to check means
// less that a new iOS release can break (risk R10).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace apk::macho {

/// Page size of iOS and of the macOS hosts we build for.
inline constexpr std::uint64_t page_size = 16 * 1024;

/// Free bytes the signers need behind the load commands: codesign and zsign add LC_CODE_SIGNATURE
/// there, over whatever lies in the way. Sections start at least this far behind the last command.
inline constexpr std::size_t signature_headroom = 256;

/// LC_BUILD_VERSION platforms.
enum class Platform : std::uint32_t {
    macos = 1,
    ios = 2,
    ios_simulator = 7,
};

// VM protections, as in <mach/vm_prot.h>.
inline constexpr std::uint32_t protection_read = 1;
inline constexpr std::uint32_t protection_write = 2;
inline constexpr std::uint32_t protection_execute = 4;

struct Section {
    std::string name;  // at most 16 characters, e.g. "__apk_info"
    std::uint64_t address = 0;
    std::uint64_t size = 0;
};

struct Segment {
    std::string name;  // at most 16 characters, e.g. "__TEXT"
    std::uint64_t address = 0;
    std::uint64_t vm_size = 0;      // bytes beyond file_size are zero filled by dyld
    std::uint64_t file_offset = 0;
    std::uint64_t file_size = 0;
    std::uint32_t protection = 0;   // used for both initprot and maxprot: we never change it
    std::vector<Section> sections;
};

struct Dylib {
    std::string install_name;
    Platform platform = Platform::macos;
    std::uint32_t version = 0;  // (major << 16) | (minor << 8) | patch, for minos and sdk
    /// LC_UUID. dyld 27 refuses a dylib without one ("missing LC_UUID load command"), so the
    /// container builder derives it from the contents and the same ELF always gives the same
    /// container.
    std::array<std::uint8_t, 16> uuid{};
    /// In file and address order: __TEXT first (at file offset 0), __LINKEDIT last.
    std::vector<Segment> segments;
};

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Bytes the Mach-O header and the load commands take. The container builder needs this before it
/// can place anything, so it is computed without writing.
std::size_t header_size(const Dylib &dylib);

/// Writes the header and the load commands over the first header_size(dylib) bytes of `image`.
/// `image` already holds the segment contents at their file offsets. Throws FormatError when the
/// layout is not one dyld can map.
void write_header(const Dylib &dylib, std::span<std::byte> image);

}  // namespace apk::macho
