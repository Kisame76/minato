// The converter's C interface for the app (APKConvert/apkconv.h).
#include "APKConvert/apkconv.h"

#include "apkconv/codesign.h"
#include "apkconv/container.h"
#include "apkconv/macho.h"
#include "apkconv/security_identity.h"

#include <TargetConditionals.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <system_error>

struct apkconv_signer {
    std::optional<apk::codesign::Identity> identity;  // none: ad hoc
    std::string identifier;
};

namespace {

#if TARGET_OS_SIMULATOR
constexpr auto platform = apk::macho::Platform::ios_simulator;
#elif TARGET_OS_IOS
constexpr auto platform = apk::macho::Platform::ios;
#else
constexpr auto platform = apk::macho::Platform::macos;
#endif

void report(const std::exception &failure, char *error, std::size_t error_size) {
    if (error != nullptr && error_size > 0) std::snprintf(error, error_size, "%s", failure.what());
}

void copy_text(const std::string &text, char *out, std::size_t size) {
    if (size > 0) std::snprintf(out, size, "%s", text.c_str());
}

/// A file mapped read-only, so that a large library is not copied just to be read.
class MappedFile {
public:
    explicit MappedFile(const char *path) {
        const int descriptor = ::open(path, O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) throw std::system_error(errno, std::generic_category(), std::format("cannot open {}", path));
        struct stat status {};
        if (::fstat(descriptor, &status) != 0 || status.st_size <= 0) {
            ::close(descriptor);
            throw std::runtime_error(std::format("{} is empty or cannot be read", path));
        }
        size_ = static_cast<std::size_t>(status.st_size);
        data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, descriptor, 0);
        ::close(descriptor);
        if (data_ == MAP_FAILED) throw std::system_error(errno, std::generic_category(), std::format("cannot map {}", path));
    }
    ~MappedFile() { ::munmap(data_, size_); }
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;

    std::span<const std::byte> bytes() const { return {static_cast<const std::byte *>(data_), size_}; }

private:
    void *data_ = nullptr;
    std::size_t size_ = 0;
};

/// Writes `bytes` to a new file next to `path` and moves it over `path` (ADR-0008).
void replace_file(const std::filesystem::path &path, std::span<const std::byte> bytes) {
    const auto temporary = path.string() + std::format(".{:08x}.tmp", std::random_device{}());
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "cannot create " + temporary);
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int saved = errno;
            ::close(descriptor);
            ::unlink(temporary.c_str());
            throw std::system_error(saved, std::generic_category(), "cannot write " + temporary);
        }
        written += static_cast<std::size_t>(count);
    }
    ::close(descriptor);
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const int saved = errno;
        ::unlink(temporary.c_str());
        throw std::system_error(saved, std::generic_category(), "cannot move the container to " + path.string());
    }
}

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void check_signature(const char *path) {
    const MappedFile file(path);
    const auto image = file.bytes();
    if (image.size() < sizeof(mach_header_64)) throw std::runtime_error("not a Mach-O file");
    mach_header_64 header;
    std::memcpy(&header, image.data(), sizeof header);
    if (header.magic != MH_MAGIC_64) throw std::runtime_error("not a thin 64-bit Mach-O file");
    std::optional<linkedit_data_command> signature;
    std::size_t offset = sizeof header;
    for (std::uint32_t index = 0; index < header.ncmds && offset + sizeof(load_command) <= image.size(); ++index) {
        load_command command;
        std::memcpy(&command, image.data() + offset, sizeof command);
        if (command.cmd == LC_CODE_SIGNATURE && offset + sizeof(linkedit_data_command) <= image.size()) {
            signature.emplace();
            std::memcpy(&*signature, image.data() + offset, sizeof *signature);
        }
        if (command.cmdsize == 0) break;
        offset += command.cmdsize;
    }
    if (!signature) throw std::runtime_error("the file has no code signature");

    const int descriptor = ::open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), std::format("cannot open {}", path));
    // For a thin file the slice starts at 0, and fs_blob_start is the signature's file offset.
    fsignatures_t signatures{};
    signatures.fs_file_start = 0;
    signatures.fs_blob_start = reinterpret_cast<void *>(static_cast<std::uintptr_t>(signature->dataoff));
    signatures.fs_blob_size = signature->datasize;
    if (::fcntl(descriptor, F_ADDFILESIGS_RETURN, &signatures) == -1) {
        const int saved = errno;
        ::close(descriptor);
        throw std::runtime_error(std::format("the kernel refuses the code signature: {} (errno {})",
                                             std::strerror(saved), saved));
    }
    char message[512] = {};
    fchecklv_t validation{};
    validation.lv_file_start = 0;
    validation.lv_error_message_size = sizeof message;
    validation.lv_error_message = message;
    if (::fcntl(descriptor, F_CHECK_LV, &validation) == -1) {
        const int saved = errno;
        ::close(descriptor);
        throw std::runtime_error(std::format("library validation refuses the container: {} ({}, errno {})", message,
                                             std::strerror(saved), saved));
    }
    ::close(descriptor);
}

/// The smallest container: a page of __TEXT with the header, and __LINKEDIT.
std::vector<std::byte> test_container() {
    using apk::macho::page_size;
    const apk::macho::Dylib dylib{
        .install_name = "@rpath/signing-check.dylib",
        .platform = platform,
        .version = 26u << 16,
        .uuid = {0x41, 0x50, 0x4b, 0x43, 0x48, 0x45, 0x43, 0x4b, 0x80, 0, 0, 0, 0, 0, 0, 1},
        .segments =
            {
                apk::macho::Segment{.name = "__TEXT",
                                    .address = 0,
                                    .vm_size = page_size,
                                    .file_offset = 0,
                                    .file_size = page_size,
                                    .protection = apk::macho::protection_read | apk::macho::protection_execute,
                                    .sections = {}},
                apk::macho::Segment{.name = "__LINKEDIT",
                                    .address = page_size,
                                    .vm_size = page_size,
                                    .file_offset = page_size,
                                    .file_size = 16,
                                    .protection = apk::macho::protection_read,
                                    .sections = {}},
            },
    };
    std::vector<std::byte> image(page_size + 16);
    apk::macho::write_header(dylib, image);
    return image;
}

}  // namespace

extern "C" apkconv_signer *apkconv_signer_create(SecIdentityRef identity, const char *chain_source,
                                                 const char *identifier, char *error, size_t error_size) {
    try {
        if (identifier == nullptr || *identifier == '\0') throw std::runtime_error("the signer needs an identifier");
        auto signer = std::make_unique<apkconv_signer>();
        signer->identifier = identifier;
        if (identity != nullptr) {
            if (chain_source == nullptr) throw std::runtime_error("signing with a certificate needs a chain source");
            const MappedFile source(chain_source);
            const auto own = apk::codesign::read_signature(source.bytes());
            signer->identity = apk::codesign::identity_from(identity, own.certificates);
            const auto &team = signer->identity->chain.front().organizational_unit;
            if (!own.team.empty() && own.team != team) {
                throw std::runtime_error(std::format(
                    "the certificate belongs to team {}, but the app is signed for team {}; iOS would not load "
                    "code signed with it",
                    team, own.team));
            }
        }
        return signer.release();
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return nullptr;
    }
}

extern "C" void apkconv_signer_free(apkconv_signer *signer) {
    delete signer;
}

extern "C" int apkconv_convert(const apkconv_signer *signer, const char *elf_path, const char *container_path,
                               apkconv_statistics *statistics, char *error, size_t error_size) {
    try {
        if (signer == nullptr || elf_path == nullptr || container_path == nullptr) {
            throw std::runtime_error("apkconv_convert needs a signer and two paths");
        }
        const std::filesystem::path output(container_path);
        auto start = std::chrono::steady_clock::now();
        auto container = [&] {
            const MappedFile elf(elf_path);
            return apk::container::convert(elf.bytes(), {.install_name = "@rpath/" + output.filename().string(),
                                                         .platform = platform});
        }();
        const auto convert_time = milliseconds_since(start);
        start = std::chrono::steady_clock::now();
        apk::codesign::sign(container.bytes, {.identifier = signer->identifier,
                                              .identity = signer->identity ? &*signer->identity : nullptr});
        const auto sign_time = milliseconds_since(start);
        replace_file(output, container.bytes);
        if (statistics != nullptr) {
            *statistics = {.convert_milliseconds = convert_time,
                           .sign_milliseconds = sign_time,
                           .container_bytes = container.bytes.size()};
        }
        return 0;
    } catch (const apk::container::Incompatible &failure) {
        if (error != nullptr && error_size > 0) std::snprintf(error, error_size, "incompatible: %s", failure.what());
        return -1;
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return -1;
    }
}

extern "C" int apkconv_check_signature(const char *path, char *error, size_t error_size) {
    try {
        check_signature(path);
        return 0;
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return -1;
    }
}

extern "C" int apkconv_check_signer(const apkconv_signer *signer, const char *directory, char *error,
                                    size_t error_size) {
    try {
        if (signer == nullptr || directory == nullptr) throw std::runtime_error("apkconv_check_signer needs a signer");
        auto image = test_container();
        apk::codesign::sign(image, {.identifier = signer->identifier,
                                    .identity = signer->identity ? &*signer->identity : nullptr});
        const auto path = std::filesystem::path(directory) /
                          std::format("signing-check-{:08x}.dylib", std::random_device{}());
        replace_file(path, image);
        try {
            check_signature(path.c_str());
        } catch (...) {
            std::filesystem::remove(path);
            throw;
        }
        std::filesystem::remove(path);
        return 0;
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return -1;
    }
}

extern "C" int apkconv_describe_certificate(const void *der, size_t size, apkconv_certificate *certificate,
                                            char *error, size_t error_size) {
    try {
        if (der == nullptr || certificate == nullptr) throw std::runtime_error("no certificate");
        const auto parsed = apk::x509::parse(std::span(static_cast<const std::byte *>(der), size));
        copy_text(parsed.common_name, certificate->common_name, sizeof certificate->common_name);
        copy_text(parsed.organizational_unit, certificate->team, sizeof certificate->team);
        certificate->not_after = parsed.not_after.time_since_epoch().count();
        return 0;
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return -1;
    }
}

extern "C" int apkconv_signing_team(const char *path, char *team, size_t team_size, char *error,
                                    size_t error_size) {
    try {
        if (path == nullptr || team == nullptr) throw std::runtime_error("apkconv_signing_team needs a path");
        const MappedFile file(path);
        copy_text(apk::codesign::read_signature(file.bytes()).team, team, team_size);
        return 0;
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return -1;
    }
}

extern "C" int apkconv_signing_certificate(const char *path, void *certificate, size_t size, size_t *length,
                                           char *error, size_t error_size) {
    try {
        if (path == nullptr || certificate == nullptr || length == nullptr) {
            throw std::runtime_error("apkconv_signing_certificate needs a path, a buffer and a length");
        }
        *length = 0;
        const MappedFile file(path);
        const auto leaf = apk::codesign::read_signature(file.bytes()).leaf;
        if (leaf.empty()) throw std::runtime_error(std::format("{} is signed ad hoc, without a certificate", path));
        *length = leaf.size();
        if (leaf.size() > size) {
            throw std::runtime_error(std::format("the certificate takes {} bytes, the buffer holds {}", leaf.size(), size));
        }
        std::memcpy(certificate, leaf.data(), leaf.size());
        return 0;
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return -1;
    }
}
