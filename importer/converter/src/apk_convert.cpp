// apk-convert: the converter as a command-line tool on the Mac (M2a, M2b).
//   apk-convert inspect <elf-file>...                 what the loader will see in an ELF file
//   apk-convert convert <elf-file> <dylib> [options]  write the container dylib (ADR-0019)
//   apk-convert sign <mach-o>... --sign <identity>    sign containers again, e.g. for another identity
//   apk-convert export-identity <signed-file> <file.p12>
//                                                     the identity in the keychain that signed the file,
//                                                     for the app on the iPhone (tools/hand-over-certificate.sh)
// Signing (ADR-0022), as the app does it on the iPhone:
//   --sign -              ad hoc, byte for byte like `codesign --sign -`
//   --sign <file.p12>     with the identity in the file; the password comes from APK_P12_PASSWORD
//                         or is asked for on the terminal (export-identity takes it the same way)
//   --identifier <id>     the signing identifier; by default the file name without its last
//                         extension, as codesign does. On iOS it must be the app's bundle ID.
//   --chain-from <file>   a signed Mach-O whose signature carries the certificate chain, e.g. the
//                         app's executable from an Xcode build (a .p12 export holds only the leaf)
// For memset_s, which clears the password where the compiler may not optimize it away.
#define __STDC_WANT_LIB_EXT1__ 1

#include "apkconv/codesign.h"
#include "apkconv/container.h"
#include "apkconv/elf.h"
#include "apkconv/security_identity.h"

#include <fcntl.h>
#include <readpassphrase.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

using apk::elf::FileType;
using apk::elf::Segment;
using apk::elf::SegmentType;

std::vector<std::byte> read_file(const std::string &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::system_error(errno, std::generic_category(), "cannot open");
    const auto size = stream.tellg();
    if (size < 0) throw std::runtime_error("cannot determine the size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("cannot read the whole file");
    return bytes;
}

std::string describe(FileType type) {
    switch (type) {
        case FileType::shared: return "shared object or position-independent executable";
        case FileType::executable: return "executable at fixed addresses";
        case FileType::relocatable: return "relocatable object";
        case FileType::core: return "core dump";
        case FileType::none: break;
    }
    return std::format("type {:#x}", static_cast<std::uint16_t>(type));
}

std::string permissions(std::uint32_t flags) {
    std::string text = "---";
    if (flags & apk::elf::segment_readable) text[0] = 'r';
    if (flags & apk::elf::segment_writable) text[1] = 'w';
    if (flags & apk::elf::segment_executable) text[2] = 'x';
    return text;
}

std::string page_verdict(const apk::elf::File &file, std::span<const std::byte> bytes) {
    // The section headers decide what a mixed p_align really needs, as in convert(); a broken table counts as none.
    try {
        const auto layout = apk::container::page_layout(file, apk::container::layout_sections(bytes));
        if (apk::elf::first_misaligned_load(file) == nullptr) {
            const auto loads = std::ranges::count(file.segments, SegmentType::load, &Segment::type);
            return std::format("yes, all {} LOAD segments fit", loads);
        }
        if (layout.shift == 0) return "yes, a 4 KiB layout with code and data 16 KiB apart (ADR-0007)";
        auto verdict = std::format("yes, a 4 KiB layout, the image moved by {:#x} (ADR-0007)", layout.shift);
        if (!layout.moved_sections.empty()) {
            verdict += std::format("; moved off 16 KiB alignment: {}", layout.moved_sections);
        }
        return verdict;
    } catch (const apk::container::Incompatible &error) {
        return std::format("no, {}", error.what());
    }
}

void inspect(const std::string &path) {
    const auto bytes = read_file(path);
    const auto file = apk::elf::read(bytes);

    std::cout << path << '\n'
              << std::format("  ELF64 little-endian, {}, {}\n", apk::elf::machine_name(file.machine),
                             describe(file.type))
              << std::format("  entry {:#x}, interpreter: {}\n", file.entry,
                             file.interpreter.empty() ? "none" : file.interpreter)
              << std::format("  {:<3}{:<14}{:>10}{:>12}{:>12}{:>12}{:>9}  {}\n", "#", "segment", "offset", "address",
                             "file size", "mem size", "align", "flags");
    for (std::size_t index = 0; index < file.segments.size(); ++index) {
        const auto &segment = file.segments[index];
        auto type = std::string(apk::elf::name(segment.type));
        if (type.empty()) type = std::format("{:#x}", static_cast<std::uint32_t>(segment.type));
        std::cout << std::format("  {:<3}{:<14}{:>#10x}{:>#12x}{:>#12x}{:>#12x}{:>#9x}  {}\n", index, type,
                                 segment.offset, segment.address, segment.file_size, segment.memory_size,
                                 segment.alignment, permissions(segment.flags));
    }
    std::cout << "  16 KiB pages: " << page_verdict(file, bytes) << '\n';

    // What the rewriter finds in the executable sections (ADR-0010, ADR-0019).
    try {
        const auto scan = apk::container::scan_code(bytes);
        std::size_t thread_pointer_reads = 0, thread_pointer_writes = 0, syscalls = 0, x18_islands = 0;
        for (const auto &site : scan.findings.sites) {
            switch (site.kind) {
                case apk::rewrite::SiteKind::thread_pointer_read: ++thread_pointer_reads; break;
                case apk::rewrite::SiteKind::thread_pointer_write: ++thread_pointer_writes; break;
                case apk::rewrite::SiteKind::syscall: ++syscalls; break;
                case apk::rewrite::SiteKind::x18:
                case apk::rewrite::SiteKind::x18_nosave: ++x18_islands; break;
            }
        }
        const auto &findings = scan.findings;
        // The same islands convert() counts: single sites, relocated LL/SC windows, jump tables.
        x18_islands += findings.exclusive_regions.size() + findings.jump_tables.size();
        std::cout << std::format(
                         "  code: {} bytes; TPIDR_EL0: {} reads, {} writes; svc #0: {}, other svc: {}, svc #imm "
                         "kept as data: {}\n",
                         scan.code_bytes, thread_pointer_reads, thread_pointer_writes, syscalls,
                         findings.other_syscalls.size(), findings.data_syscalls.size())
                  << std::format("  x18: {} reads, {} write{}, {} saved in register dumps; islands: {}, windows in "
                                 "place: {}, refused: {}",
                                 findings.x18_reads.size(), findings.x18_writes.size(),
                                 findings.x18_writes.size() == 1 ? "" : "s", findings.x18_saves.size(), x18_islands,
                                 findings.in_place_windows.size(), findings.x18_unsupported.size());
        if (!findings.x18_unsupported.empty()) {
            std::cout << " (" << apk::container::x18_refusal(findings.x18_unsupported, apk::elf::read_symbols(bytes),
                                                             apk::container::layout_sections(bytes))
                      << ')';
        }
        std::cout << '\n';
    } catch (const apk::container::Incompatible &error) {
        std::cout << "  code: " << error.what() << '\n';
    }
}

void write_file(const std::string &path, const std::vector<std::byte> &bytes) {
    // Replace, never overwrite in place: the kernel caches signatures per file (ADR-0008).
    const auto temporary = path + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) throw std::runtime_error("cannot write " + temporary);
    }
    std::filesystem::rename(temporary, path);
}

/// Writes key material: a new file only the user can read.
void write_private_file(const std::string &path, std::span<const std::byte> bytes) {
    std::filesystem::remove(path);
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "cannot write " + path);
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int failure = errno;
            ::close(descriptor);
            throw std::system_error(failure, std::generic_category(), "cannot write " + path);
        }
        written += static_cast<std::size_t>(count);
    }
    if (::close(descriptor) != 0) throw std::system_error(errno, std::generic_category(), "cannot write " + path);
}

apk::macho::Platform parse_platform(const std::string &name) {
    if (name == "macos") return apk::macho::Platform::macos;
    if (name == "ios") return apk::macho::Platform::ios;
    if (name == "ios-simulator") return apk::macho::Platform::ios_simulator;
    throw std::runtime_error("unknown platform " + name + " (macos, ios, ios-simulator)");
}

/// How to sign, from the options.
struct Signing {
    bool requested = false;
    std::string identity;  // "-" for ad hoc, or a .p12 file
    std::string identifier;
    std::string chain_from;
    std::optional<apk::codesign::Identity> loaded;  // from the .p12 file
};

std::string password() {
    if (const char *value = std::getenv("APK_P12_PASSWORD")) return value;
    char buffer[256];
    if (::readpassphrase("Password of the .p12 file: ", buffer, sizeof buffer, RPP_REQUIRE_TTY) == nullptr) {
        throw std::runtime_error("no password for the .p12 file: set APK_P12_PASSWORD or run on a terminal");
    }
    std::string result(buffer);
    ::memset_s(buffer, sizeof buffer, 0, sizeof buffer);
    return result;
}

/// Takes the signing options out of `arguments`; what is left are the other arguments.
Signing signing_options(std::vector<std::string> &arguments) {
    Signing signing;
    std::vector<std::string> rest;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto &argument = arguments[index];
        const bool has_value = index + 1 < arguments.size();
        if (argument == "--sign" && has_value) {
            signing.requested = true;
            signing.identity = arguments[++index];
        } else if (argument == "--identifier" && has_value) {
            signing.identifier = arguments[++index];
        } else if (argument == "--chain-from" && has_value) {
            signing.chain_from = arguments[++index];
        } else {
            rest.push_back(argument);
        }
    }
    arguments = rest;
    if (!signing.requested && (!signing.identifier.empty() || !signing.chain_from.empty())) {
        throw std::runtime_error("--identifier and --chain-from need --sign");
    }
    if (signing.requested && signing.identity != "-") {
        std::vector<apk::der::Bytes> candidates;
        if (!signing.chain_from.empty()) {
            candidates = apk::codesign::read_signature(read_file(signing.chain_from)).certificates;
        }
        signing.loaded = apk::codesign::identity_from_p12(read_file(signing.identity), password(), candidates);
    }
    return signing;
}

/// Signs `image` for the file `path`; says how, for the report.
std::string sign(std::vector<std::byte> &image, const Signing &signing, const std::string &path) {
    const auto identifier =
        signing.identifier.empty() ? std::filesystem::path(path).stem().string() : signing.identifier;
    const auto *identity = signing.loaded ? &*signing.loaded : nullptr;
    const auto start = std::chrono::steady_clock::now();
    apk::codesign::sign(image, {.identifier = identifier, .identity = identity});
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
    if (identity == nullptr) return std::format("signed ad hoc as {} in {:.2f} ms", identifier, elapsed.count());
    const auto &leaf = identity->chain.front();
    return std::format("signed as {} by \"{}\" (team {}) in {:.2f} ms", identifier, leaf.common_name,
                       leaf.organizational_unit, elapsed.count());
}

int convert(std::vector<std::string> arguments) {
    const auto signing = signing_options(arguments);
    if (arguments.size() < 3) {
        std::cerr << "usage: apk-convert convert <elf-file> <dylib> [--platform macos|ios|ios-simulator]\n"
                     "                           [--sign -|<file.p12>] [--identifier <id>] [--chain-from <file>]\n";
        return 2;
    }
    const auto &input = arguments[1];
    const auto &output = arguments[2];
    apk::container::Options options{
        .install_name = "@rpath/" + std::filesystem::path(output).filename().string(),
    };
    for (std::size_t index = 3; index < arguments.size(); ++index) {
        if (arguments[index] == "--platform" && index + 1 < arguments.size()) {
            options.platform = parse_platform(arguments[++index]);
        } else {
            throw std::runtime_error("unknown option " + arguments[index]);
        }
    }

    const auto bytes = read_file(input);
    const auto start = std::chrono::steady_clock::now();
    auto container = apk::container::convert(bytes, options);
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
    const auto signature = signing.requested ? sign(container.bytes, signing, output) : "unsigned";
    write_file(output, container.bytes);

    const auto &report = container.report;
    std::cout << std::format("{} -> {} ({} bytes, {})\n", input, output, container.bytes.size(), signature)
              << std::format("  {}, {} bytes of code scanned\n", report.is_static ? "static" : "dynamic",
                             report.code_bytes)
              << std::format("  rewritten: {} thread pointer reads, {} writes, {} syscalls ({} bytes of stubs)\n",
                             report.thread_pointer_reads, report.thread_pointer_writes, report.syscalls,
                             report.stub_bytes)
              << std::format("  x18: {} island{} ({} bytes of island table), of them {} only writing x18, {} saving "
                             "it in a register dump\n",
                             report.x18_islands, report.x18_islands == 1 ? "" : "s", report.island_table_bytes,
                             report.x18_writes, report.x18_saves)
              << std::format("  relocations: {}, ELF image at {:#x}\n", report.relocations, report.image_offset)
              << std::format("  converted in {:.2f} ms\n", elapsed.count());
    return 0;
}

int sign_files(std::vector<std::string> arguments) {
    const auto signing = signing_options(arguments);
    if (arguments.size() < 2 || !signing.requested) {
        std::cerr << "usage: apk-convert sign <mach-o>... --sign -|<file.p12> [--identifier <id>] [--chain-from <file>]\n";
        return 2;
    }
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        auto image = read_file(arguments[index]);
        const auto how = sign(image, signing, arguments[index]);
        write_file(arguments[index], image);
        std::cout << std::format("{}: {}\n", arguments[index], how);
    }
    return 0;
}

/// Writes the identity in the keychain that signed a file, e.g. the app Xcode built, to a .p12
/// file, for the app on the iPhone to import (tools/hand-over-certificate.sh). macOS asks whether
/// to allow the export of the private key.
int export_identity(const std::vector<std::string> &arguments) {
    if (arguments.size() != 3) {
        std::cerr << "usage: apk-convert export-identity <signed-file> <file.p12>\n";
        return 2;
    }
    const auto signature = apk::codesign::read_signature(read_file(arguments[1]));
    if (signature.leaf.empty()) throw std::runtime_error(arguments[1] + " is signed ad hoc, without a certificate");
    const auto certificate = apk::x509::parse(signature.leaf);
    write_private_file(arguments[2], apk::codesign::keychain_identity_as_p12(certificate, password()));
    const std::chrono::year_month_day until{std::chrono::floor<std::chrono::days>(certificate.not_after)};
    std::cout << std::format("{}: the identity of team {}, valid until {:04}-{:02}-{:02}\n", arguments[2],
                             certificate.organizational_unit, static_cast<int>(until.year()),
                             static_cast<unsigned>(until.month()), static_cast<unsigned>(until.day()));
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    if (!arguments.empty() && (arguments[0] == "convert" || arguments[0] == "sign" || arguments[0] == "export-identity")) {
        try {
            if (arguments[0] == "export-identity") return export_identity(arguments);
            return arguments[0] == "convert" ? convert(arguments) : sign_files(arguments);
        } catch (const apk::container::Incompatible &error) {
            std::cerr << std::format("apk-convert: incompatible: {}\n", error.what());
            return 1;
        } catch (const std::exception &error) {
            std::cerr << std::format("apk-convert: {}\n", error.what());
            return 1;
        }
    }
    if (arguments.size() < 2 || arguments[0] != "inspect") {
        std::cerr << "usage: apk-convert inspect <elf-file>...\n"
                     "       apk-convert convert <elf-file> <dylib> [--platform macos|ios|ios-simulator]\n"
                     "                           [--sign -|<file.p12>] [--identifier <id>] [--chain-from <file>]\n"
                     "       apk-convert sign <mach-o>... --sign -|<file.p12> [--identifier <id>] [--chain-from <file>]\n"
                     "       apk-convert export-identity <signed-file> <file.p12>\n";
        return 2;
    }
    int status = 0;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (index > 1) std::cout << '\n';
        try {
            inspect(arguments[index]);
        } catch (const std::exception &error) {
            std::cerr << std::format("apk-convert: {}: {}\n", arguments[index], error.what());
            status = 1;
        }
    }
    return status;
}
