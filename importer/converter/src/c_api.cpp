// The converter's C interface for the app (APKConvert/apkconv.h).
#include "APKConvert/apkconv.h"
#include "APKRuntime/container_format.h"

#include "apkconv/codesign.h"
#include "apkconv/container.h"
#include "apkconv/macho.h"
#include "apkconv/pkcs12.h"
#include "apkconv/security_identity.h"

#include <TargetConditionals.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif
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
        if (::fstat(descriptor, &status) != 0) {
            const int failure = errno;
            ::close(descriptor);
            throw std::system_error(failure, std::generic_category(), std::format("cannot read {}", path));
        }
        if (status.st_size <= 0) {
            ::close(descriptor);
            // The file's own fault, not the reader's: an empty library cannot run either (ADR-0032).
            throw apk::container::Incompatible(std::format("{} is empty", path));
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

/// Where the test container's function sits: at the end of its __TEXT page, after the header.
constexpr std::size_t test_function_offset = apk::macho::page_size - 16;

/// The UUID of the test container made with `tag`.
std::array<std::uint8_t, 16> test_container_uuid(std::uint32_t tag) {
    return {0x41, 0x50, 0x4b, 0x43, 0x48, 0x45, 0x43, 0x4b, 0x80, 0, 0, 0, static_cast<std::uint8_t>(tag >> 24),
            static_cast<std::uint8_t>(tag >> 16), static_cast<std::uint8_t>(tag >> 8), static_cast<std::uint8_t>(tag)};
}

/// The smallest container: a page of __TEXT with the header and a function that returns 42, and
/// __LINKEDIT. `tag` makes its install name and UUID unique, so that dyld loads each one anew.
std::vector<std::byte> test_container(std::uint32_t tag) {
    using apk::macho::page_size;
    const apk::macho::Dylib dylib{
        .install_name = std::format("@rpath/signing-check-{:08x}.dylib", tag),
        .platform = platform,
        .version = 26u << 16,
        .uuid = test_container_uuid(tag),
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
    const std::uint32_t function[] = {0x5280'0540, 0xd65f'03c0};  // mov w0, #42; ret
    std::memcpy(image.data() + test_function_offset, function, sizeof function);
    return image;
}

/// The image dyld loaded with `uuid`, found by its LC_UUID; nullptr when there is none.
const mach_header_64 *loaded_image(const std::array<std::uint8_t, 16> &uuid) {
    for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
        const auto *header = reinterpret_cast<const mach_header_64 *>(_dyld_get_image_header(index));
        if (header == nullptr || header->magic != MH_MAGIC_64) continue;
        const auto *command = reinterpret_cast<const std::uint8_t *>(header + 1);
        for (std::uint32_t number = 0; number < header->ncmds; ++number) {
            load_command current;
            std::memcpy(&current, command, sizeof current);
            if (current.cmd == LC_UUID) {
                uuid_command identifier;
                std::memcpy(&identifier, command, sizeof identifier);
                if (std::memcmp(identifier.uuid, uuid.data(), uuid.size()) == 0) return header;
                break;
            }
            command += current.cmdsize;
        }
    }
    return nullptr;
}

/// The code signature of the app's own executable, `chain_source`: its certificates complete the
/// chain of an imported certificate, and its team is the one iOS accepts.
apk::codesign::Signature app_signature(const char *chain_source) {
    if (chain_source == nullptr) throw std::runtime_error("signing with a certificate needs a chain source");
    const MappedFile source(chain_source);
    return apk::codesign::read_signature(source.bytes());
}

void check_team(const apk::codesign::Identity &identity, const apk::codesign::Signature &app) {
    const auto &team = identity.chain.front().organizational_unit;
    if (!app.team.empty() && app.team != team) {
        throw std::runtime_error(std::format(
            "the certificate belongs to team {}, but the app is signed for team {}; iOS would not load "
            "code signed with it",
            team, app.team));
    }
}

}  // namespace

extern "C" apkconv_signer *apkconv_signer_create(SecIdentityRef identity, const char *chain_source,
                                                 const char *identifier, char *error, size_t error_size) {
    try {
        if (identifier == nullptr || *identifier == '\0') throw std::runtime_error("the signer needs an identifier");
        auto signer = std::make_unique<apkconv_signer>();
        signer->identifier = identifier;
        if (identity != nullptr) {
            const auto app = app_signature(chain_source);
            signer->identity = apk::codesign::identity_from(identity, app.certificates);
            check_team(*signer->identity, app);
        }
        return signer.release();
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return nullptr;
    }
}

extern "C" apkconv_signer *apkconv_signer_create_with_key(SecKeyRef private_key, const void *certificate,
                                                          size_t certificate_size, const char *chain_source,
                                                          const char *identifier, char *error, size_t error_size) {
    try {
        if (identifier == nullptr || *identifier == '\0') throw std::runtime_error("the signer needs an identifier");
        if (private_key == nullptr || certificate == nullptr) {
            throw std::runtime_error("apkconv_signer_create_with_key needs a key and a certificate");
        }
        auto signer = std::make_unique<apkconv_signer>();
        signer->identifier = identifier;
        const auto app = app_signature(chain_source);
        const auto leaf = apk::x509::parse(std::span(static_cast<const std::byte *>(certificate), certificate_size));
        signer->identity = apk::codesign::identity_with_key(private_key, apk::codesign::complete_chain(leaf, app.certificates));
        check_team(*signer->identity, app);
        return signer.release();
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return nullptr;
    }
}

extern "C" void apkconv_signer_free(apkconv_signer *signer) {
    delete signer;
}

extern "C" uint32_t apkconv_container_version(void) {
    return APK_CONTAINER_VERSION;
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
            try {
                return apk::container::convert(elf.bytes(), {.install_name = "@rpath/" + output.filename().string(),
                                                             .platform = platform});
            } catch (const std::bad_alloc &) {
                throw;
            } catch (const apk::container::Incompatible &) {
                throw;
            } catch (const std::exception &failure) {
                // Whatever else the conversion finds is the file's too (elf::FormatError, RewriteError,
                // macho::FormatError): it cannot run on iOS as it is, however often it is tried (ADR-0032).
                throw apk::container::Incompatible(failure.what());
            }
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

extern "C" int apkconv_refusals(const char *elf_path, char *reasons, size_t reasons_size, size_t *reasons_needed,
                                char *error, size_t error_size) {
    try {
        if (elf_path == nullptr || reasons == nullptr) {
            throw std::runtime_error("apkconv_refusals needs a path and a buffer");
        }
        std::vector<std::string> found;
        try {
            const MappedFile elf(elf_path);
            found = apk::container::refusals(elf.bytes());
        } catch (const apk::container::Incompatible &empty) {
            found = {empty.what()};  // an empty file: its one reason, as apkconv_convert gives it
        }
        std::string text;
        for (auto reason : found) {
            // One reason is one line of printable text, also an empty file's, which carries the path.
            for (auto &letter : reason) {
                if (static_cast<unsigned char>(letter) < 0x20 || letter == 0x7f) letter = ' ';
            }
            text += (text.empty() ? "" : "\n") + reason;
        }
        if (reasons_needed != nullptr) *reasons_needed = text.size() + 1;
        copy_text(text, reasons, reasons_size);
        return static_cast<int>(found.size());
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return -1;
    }
}

extern "C" int apkconv_counts_of(const char *elf_path, apkconv_counts *counts, char *error, size_t error_size) {
    try {
        if (elf_path == nullptr || counts == nullptr) {
            throw std::runtime_error("apkconv_counts_of needs a path and a result");
        }
        const MappedFile elf(elf_path);
        const auto found = apk::container::counts(elf.bytes());
        *counts = {.svc_rewritten = found.svc_rewritten,
                   .svc_data = found.svc_data,
                   .svc_in_data = found.svc_in_data,
                   .undefined_words = found.undefined_words,
                   .x18_islands = found.x18_islands,
                   .renames_by_symbol = found.renames_by_symbol,
                   .renames_by_unwind = found.renames_by_unwind};
        return 0;
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
        auto image = test_container(std::random_device{}());
        apk::codesign::sign(image, {.identifier = signer->identifier,
                                    .identity = signer->identity ? &*signer->identity : nullptr});
        const auto path = std::filesystem::path(directory) /
                          std::format("signing-check-{:08x}.dylib", std::random_device{}());
        replace_file(path, image);
        try {
            check_signature(path.c_str());
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            throw;
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
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
        // The INTEGER's contents without the zero bytes DER puts in front to keep it positive.
        auto serial = apk::der::read(parsed.serial_number).contents;
        while (serial.size() > 1 && serial.front() == std::byte{0}) serial = serial.subspan(1);
        std::string serial_text;
        for (const auto byte : serial) serial_text += std::format("{:02X}", std::to_integer<unsigned>(byte));
        copy_text(serial_text, certificate->serial_number, sizeof certificate->serial_number);
        certificate->not_before = parsed.not_before.time_since_epoch().count();
        certificate->not_after = parsed.not_after.time_since_epoch().count();
        return 0;
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return -1;
    }
}

extern "C" int apkconv_self_test(const apkconv_signer *signer, const char *directory, apkconv_self_test_result *result,
                                 char *error, size_t error_size) {
    try {
        if (signer == nullptr || directory == nullptr || result == nullptr) {
            throw std::runtime_error("apkconv_self_test needs a signer, a directory and a result");
        }
        *result = {};
        const std::uint32_t tag = std::random_device{}();
        auto image = test_container(tag);
        const auto signing = std::chrono::steady_clock::now();
        apk::codesign::sign(image, {.identifier = signer->identifier,
                                    .identity = signer->identity ? &*signer->identity : nullptr});
        result->sign_milliseconds = milliseconds_since(signing);

        const auto path = std::filesystem::path(directory) / std::format("self-test-{:08x}.dylib", tag);
        replace_file(path, image);
        const auto loading = std::chrono::steady_clock::now();
        const mach_header_64 *header = nullptr;
        try {
            check_signature(path.c_str());
            if (::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL) == nullptr) {
                throw std::runtime_error(std::format("dlopen refuses the test container: {}", ::dlerror()));
            }
            header = loaded_image(test_container_uuid(tag));
            if (header == nullptr) throw std::runtime_error("dlopen loaded the test container, but dyld does not list it");
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            throw;
        }
        // The mapping stays when the file goes.
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        result->load_milliseconds = milliseconds_since(loading);

        using TestFunction = int (*)();
        const auto address = reinterpret_cast<std::uintptr_t>(header) + test_function_offset;
#if __has_feature(ptrauth_calls)
        const auto function = ptrauth_sign_unauthenticated(reinterpret_cast<TestFunction>(address), ptrauth_key_function_pointer, 0);
#else
        const auto function = reinterpret_cast<TestFunction>(address);
#endif
        result->value = function();
        if (result->value != 42) {
            throw std::runtime_error(std::format("the test container's function returned {} instead of 42", result->value));
        }
        return 0;
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return -1;
    }
}

extern "C" int apkconv_read_unencrypted_pkcs12(const void *pkcs12, size_t size, void *key, size_t key_size,
                                               size_t *key_length, void *certificate, size_t certificate_size,
                                               size_t *certificate_length, char *error, size_t error_size) {
    try {
        if (pkcs12 == nullptr || key == nullptr || key_length == nullptr || certificate == nullptr ||
            certificate_length == nullptr) {
            throw std::runtime_error("apkconv_read_unencrypted_pkcs12 needs a file and two buffers");
        }
        *key_length = 0;
        *certificate_length = 0;
        const auto contents = apk::pkcs12::read_unencrypted(std::span(static_cast<const std::byte *>(pkcs12), size));
        *key_length = contents.rsa_private_key.size();
        *certificate_length = contents.certificate.size();
        if (contents.rsa_private_key.size() > key_size || contents.certificate.size() > certificate_size) {
            throw std::runtime_error(std::format("the buffers are too small: the key needs {} bytes, the certificate {}",
                                                 contents.rsa_private_key.size(), contents.certificate.size()));
        }
        std::memcpy(key, contents.rsa_private_key.data(), contents.rsa_private_key.size());
        std::memcpy(certificate, contents.certificate.data(), contents.certificate.size());
        return 0;
    } catch (const apk::pkcs12::EncryptedError &failure) {
        report(failure, error, error_size);
        return 1;
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

extern "C" int apkconv_signing_chain(const char *path, char *names, size_t size, char *error, size_t error_size) {
    try {
        if (path == nullptr || names == nullptr) throw std::runtime_error("apkconv_signing_chain needs a path and a buffer");
        const MappedFile file(path);
        std::string text;
        for (const auto &certificate : apk::codesign::read_signature(file.bytes()).certificates) {
            const auto name = apk::x509::parse(certificate).common_name;
            if (!text.empty()) text += '\n';
            text += name.empty() ? "(no common name)" : name;
        }
        copy_text(text, names, size);
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

namespace {

// ADR-0008 amendment, step 0: a container in protection runs, as the Unity test APK v2's libil2cpp.so needs them: __TEXT
// (r-x, the header), __DATA (rw-), __TEXT_1 (r-x, the function), __CONST_1 (r--), __DATA_1 (rw-), __LINKEDIT.
constexpr std::uint64_t runs_data = 0x4000, runs_code = 0x8000, runs_const = 0xc000, runs_data_1 = 0x10000,
                        runs_linkedit = 0x14000;
constexpr std::uint32_t runs_const_value = 1000, runs_data_start = 7, runs_data_1_start = 5;

std::vector<std::byte> segment_runs_container(std::uint32_t tag) {
    using apk::macho::page_size;
    using apk::macho::protection_execute;
    using apk::macho::protection_read;
    using apk::macho::protection_write;
    auto segment = [](const char *name, std::uint64_t address, std::uint32_t protection, std::uint64_t file_size) {
        return apk::macho::Segment{.name = name,
                                   .address = address,
                                   .vm_size = page_size,
                                   .file_offset = address,
                                   .file_size = file_size,
                                   .protection = protection,
                                   .sections = {}};
    };
    const apk::macho::Dylib dylib{
        .install_name = std::format("@rpath/segment-runs-{:08x}.dylib", tag),
        .platform = platform,
        .version = 26u << 16,
        .uuid = test_container_uuid(tag ^ 0x5eu),
        .segments = {segment("__TEXT", 0, protection_read | protection_execute, page_size),
                     segment("__DATA", runs_data, protection_read | protection_write, page_size),
                     segment("__TEXT_1", runs_code, protection_read | protection_execute, page_size),
                     segment("__CONST_1", runs_const, protection_read, page_size),
                     segment("__DATA_1", runs_data_1, protection_read | protection_write, page_size),
                     segment("__LINKEDIT", runs_linkedit, protection_read, 16)},
    };
    std::vector<std::byte> image(runs_linkedit + 16);
    apk::macho::write_header(dylib, image);
    // At __TEXT_1: reads __CONST_1, counts in __DATA_1 and __DATA, returns const + data_1 + 10 * data.
    const std::uint32_t function[] = {
        0x10020001,  // adr x1, . + 0x4000      (__CONST_1)
        0xb9400020,  // ldr w0, [x1]
        0x1003ffc2,  // adr x2, . + 0x7ff8      (__DATA_1)
        0xb9400043,  // ldr w3, [x2]
        0x11000463,  // add w3, w3, #1
        0xb9000043,  // str w3, [x2]
        0x10fdff44,  // adr x4, . - 0x4018      (__DATA)
        0xb9400085,  // ldr w5, [x4]
        0x110004a5,  // add w5, w5, #1
        0xb9000085,  // str w5, [x4]
        0x0b030000,  // add w0, w0, w3
        0x52800146,  // mov w6, #10
        0x1b0600a0,  // madd w0, w5, w6, w0
        0xd65f03c0,  // ret
    };
    std::memcpy(image.data() + runs_code, function, sizeof function);
    std::memcpy(image.data() + runs_const, &runs_const_value, sizeof runs_const_value);
    std::memcpy(image.data() + runs_data, &runs_data_start, sizeof runs_data_start);
    std::memcpy(image.data() + runs_data_1, &runs_data_1_start, sizeof runs_data_1_start);
    return image;
}

std::string protection_at(std::uintptr_t address) {
    vm_address_t region = address;
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                     reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS || region > address) {
        return "unmapped";
    }
    return std::format("{}{}{}", (info.protection & VM_PROT_READ) ? 'r' : '-',
                       (info.protection & VM_PROT_WRITE) ? 'w' : '-', (info.protection & VM_PROT_EXECUTE) ? 'x' : '-');
}

}  // namespace

extern "C" int apkconv_segment_runs_spike(const apkconv_signer *signer, const char *directory, char *report_text,
                                          size_t report_size, char *error, size_t error_size) {
    try {
        if (signer == nullptr || directory == nullptr || report_text == nullptr) {
            throw std::runtime_error("apkconv_segment_runs_spike needs a signer, a directory and a report");
        }
        const std::uint32_t tag = std::random_device{}();
        auto image = segment_runs_container(tag);
        apk::codesign::sign(image, {.identifier = signer->identifier,
                                    .identity = signer->identity ? &*signer->identity : nullptr});
        const auto path = std::filesystem::path(directory) / std::format("segment-runs-{:08x}.dylib", tag);
        replace_file(path, image);
        const auto loading = std::chrono::steady_clock::now();
        const mach_header_64 *header = nullptr;
        try {
            check_signature(path.c_str());
            if (::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL) == nullptr) {
                throw std::runtime_error(std::format("dlopen refuses the container: {}", ::dlerror()));
            }
            header = loaded_image(test_container_uuid(tag ^ 0x5eu));
            if (header == nullptr) throw std::runtime_error("dlopen loaded the container, but dyld does not list it");
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            throw;
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        const auto load_time = milliseconds_since(loading);

        const auto base = reinterpret_cast<std::uintptr_t>(header);
        std::string text = std::format("dlopen {:.2f} ms; mapped:", load_time);
        for (const auto &[name, offset] : {std::pair{"__TEXT", std::uint64_t{0}}, {"__DATA", runs_data},
                                           {"__TEXT_1", runs_code}, {"__CONST_1", runs_const},
                                           {"__DATA_1", runs_data_1}, {"__LINKEDIT", runs_linkedit}}) {
            text += std::format(" {} {}", name, protection_at(base + offset));
        }
        using Function = int (*)();
#if __has_feature(ptrauth_calls)
        const auto function = ptrauth_sign_unauthenticated(reinterpret_cast<Function>(base + runs_code),
                                                           ptrauth_key_function_pointer, 0);
#else
        const auto function = reinterpret_cast<Function>(base + runs_code);
#endif
        const int first = function();
        const int second = function();
        text += std::format("; __TEXT_1's function: {}, {} (expected 1086, 1097)", first, second);
        copy_text(text, report_text, report_size);
        return first == 1086 && second == 1097 ? 0 : 1;
    } catch (const std::exception &failure) {
        report(failure, error, error_size);
        return -1;
    }
}
