// The host side of the system pickers for apk-run (ADR-0057 steps b to d): what a host does when the guest's pick
// request arrives and the user has chosen. The files and folders it writes are in the app's data folder and in
// Download, which the (hostile) app can write too: it may leave links, planted files and loops there. So nothing here
// opens a path. The data folder and Download are opened once from the host's own paths; everything below them is
// reached with openat and friends from a directory descriptor, folders with O_NOFOLLOW (mkdirat when missing), files
// created with O_CREAT|O_EXCL|O_NOFOLLOW (EEXIST is the only way a name is "busy": never stat or exists), the manifest
// written to "<id>.partial" (a planted one is unlinked, not followed) and renamed on the same descriptor. A link where
// a folder should be makes the answer a cancel. The contract for the iOS host is the comment of APKRT_EVENT_PICK_RESULT
// in guest_interface.h; tests/pick_host_test.cpp plays the attacks.
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pick_host {

/// What the user picks and where the guest sees it: host paths, the host's own.
struct Config {
    std::vector<std::string> files, photos;
    /// The folder the guest sees as /storage/emulated/0/Download, and the app's data folder.
    std::string download, data;
    /// A save goes to Download; a folder the user opens, relative to /storage/emulated/0 ("" for none).
    bool create = false;
    std::string tree;
};

/// A closed-on-exit directory descriptor.
class Dir {
public:
    Dir() = default;
    explicit Dir(int fd) : fd_(fd) {}
    Dir(Dir &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Dir &operator=(Dir &&other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    Dir(const Dir &) = delete;
    Dir &operator=(const Dir &) = delete;
    ~Dir() { reset(); }
    int fd() const { return fd_; }
    bool ok() const { return fd_ >= 0; }

private:
    void reset() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    int fd_ = -1;
};

/// The host's own folder, opened by its path (links in it are the host's).
inline Dir open_base(const std::string &path) { return Dir(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)); }

/// The folder `name` below `parent`, made when missing; never through a link.
inline Dir open_sub(const Dir &parent, const char *name) {
    if (!parent.ok()) return Dir();
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int fd = ::openat(parent.fd(), name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) return Dir(fd);
        if (errno != ENOENT) return Dir();
        if (::mkdirat(parent.fd(), name, 0755) != 0 && errno != EEXIST) return Dir();
    }
    return Dir();
}

/// The text of a file name that is plain for the guest (guest_interface.h): a byte that is no UTF-8, a control, a
/// separator, a bidirectional or zero-width or other formatting character, a "/" become "_"; at most `limit` bytes,
/// cut between code points, the extension (up to 16 bytes) kept; "" "." ".." become "Untitled".
inline std::string plain_name(const std::string &name, std::size_t limit = 255) {
    std::string clean;
    for (std::size_t i = 0; i < name.size();) {
        const unsigned char lead = static_cast<unsigned char>(name[i]);
        std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 0;
        std::uint32_t point = lead < 0x80 ? lead : length == 2 ? lead & 0x1F : length == 3 ? lead & 0x0F : lead & 0x07;
        bool valid = length != 0 && i + length <= name.size();
        for (std::size_t k = 1; valid && k < length; ++k) {
            const unsigned char next = static_cast<unsigned char>(name[i + k]);
            valid = (next & 0xC0) == 0x80;
            point = point << 6 | (next & 0x3F);
        }
        valid = valid && (length != 2 || point >= 0x80) && (length != 3 || point >= 0x800) &&
                (length != 4 || (point >= 0x10000 && point <= 0x10FFFF)) && !(point >= 0xD800 && point <= 0xDFFF);
        const bool plain = valid && point >= 0x20 && point != 0x7F && !(point >= 0x80 && point <= 0x9F) &&
                           point != '/' && !(point >= 0x200B && point <= 0x200F) &&
                           !(point >= 0x2028 && point <= 0x202E) && !(point >= 0x2060 && point <= 0x206F) &&
                           point != 0xFEFF && !(point >= 0xFFF9 && point <= 0xFFFB);
        if (plain) {
            clean.append(name, i, length);
            i += length;
        } else {
            clean += '_';
            i += valid ? length : 1;
        }
    }
    if (clean.empty() || clean == "." || clean == "..") return "Untitled";
    if (clean.size() <= limit) return clean;
    const std::size_t dot = clean.rfind('.');
    const std::string extension = dot != std::string::npos && dot > 0 && clean.size() - dot <= 16 ? clean.substr(dot) : "";
    std::size_t keep = limit - extension.size();
    while (keep > 0 && (static_cast<unsigned char>(clean[keep]) & 0xC0) == 0x80) --keep;
    return clean.substr(0, keep) + extension;
}

/// Candidate `n` for a name: the name itself for 0, else "<stem> (<n>)<extension>", cut to 255 bytes.
inline std::string numbered(const std::string &name, int n) {
    if (n == 0) return name;
    const std::size_t dot = name.rfind('.');
    const bool has_extension = dot != std::string::npos && dot > 0 && name.size() - dot <= 16;
    const std::string suffix = " (" + std::to_string(n) + ")";
    const std::string extension = has_extension ? name.substr(dot) : "";
    return plain_name(name.substr(0, has_extension ? dot : name.size()), 255 - extension.size() - suffix.size()) +
           suffix + extension;
}

/// Creates a file that did not exist in `dir`: the first free of the name's numbered forms, made with O_EXCL; the
/// file's descriptor, and in `chosen` its name; -1 when it cannot be made (not for a busy name).
inline int create_new(const Dir &dir, const std::string &name, std::string &chosen) {
    for (int n = 0; n < 10000; ++n) {
        chosen = numbered(name, n);
        const int fd = ::openat(dir.fd(), chosen.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    return -1;
}

inline bool write_all(int fd, const char *data, std::size_t size) {
    while (size > 0) {
        const ssize_t written = ::write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

/// Reads a whole host file (the host's own path); false when it cannot.
inline bool read_file(const std::string &path, std::string &out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[65536];
    out.clear();
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof buffer);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            ::close(fd);
            return got == 0;
        }
        out.append(buffer, static_cast<std::size_t>(got));
    }
}

/// The size of a JPEG (its first start-of-frame marker) or PNG picture, 0 and 0 for another kind.
inline void picture_size(const std::string &content, std::uint32_t &width, std::uint32_t &height) {
    width = height = 0;
    const auto at = [&](std::size_t i) -> std::uint32_t {
        return i < content.size() ? static_cast<unsigned char>(content[i]) : 0;
    };
    if (content.size() >= 24 && at(0) == 0x89 && at(1) == 'P') {
        width = at(16) << 24 | at(17) << 16 | at(18) << 8 | at(19);
        height = at(20) << 24 | at(21) << 16 | at(22) << 8 | at(23);
        return;
    }
    for (std::size_t i = 2; content.size() > 2 && at(0) == 0xFF && at(1) == 0xD8 && i + 9 < content.size();) {
        if (at(i) != 0xFF) return;
        const std::uint32_t marker = at(i + 1);
        if (marker >= 0xC0 && marker <= 0xC2) {
            height = at(i + 5) << 8 | at(i + 6);
            width = at(i + 7) << 8 | at(i + 8);
            return;
        }
        i += 2 + (at(i + 2) << 8 | at(i + 3));
    }
}

class Host {
public:
    explicit Host(Config config) : config_(std::move(config)) {}

    /// ACTION_OPEN_DOCUMENT or GET_CONTENT: the first file, or all of them when `several`, cloned into Download.
    bool documents(std::uint64_t id, bool several, std::uint64_t &count) {
        const Dir download = open_base(config_.download);
        return clone_all(id, config_.files, download, several ? ~0ull : 1, false, count);
    }

    /// The photo picker: up to `maximum` files cloned into <data>/.minato/photos, with a manifest line of metadata.
    bool photos(std::uint64_t id, std::uint64_t maximum, std::uint64_t &count) {
        const Dir data = open_base(config_.data);
        const Dir minato = open_sub(data, ".minato");
        const Dir folder = open_sub(minato, "photos");
        return clone_all(id, config_.photos, folder, maximum, true, count);
    }

    /// ACTION_CREATE_DOCUMENT (`title` not empty: the file is made in Download under it) or OPEN_DOCUMENT_TREE (the
    /// folder is --pick-tree's): the manifest is the place, relative to /storage/emulated/0.
    bool location(std::uint64_t id, const std::string &title, std::uint64_t &count) {
        count = 1;
        if (title.empty()) return write_manifest(id, config_.tree + "\n");
        const Dir download = open_base(config_.download);
        std::string chosen;
        const int fd = download.ok() ? create_new(download, plain_name(title), chosen) : -1;
        if (fd < 0) return false;
        ::close(fd);
        return write_manifest(id, "Download/" + chosen + "\n");
    }

private:
    Config config_;
    /// The clone's name of each file picked so far (per folder); the answers run one after the other in a session.
    std::map<std::string, std::string> clones_;

    bool clone_all(std::uint64_t id, const std::vector<std::string> &files, const Dir &folder, std::uint64_t limit,
                   bool photo, std::uint64_t &count) {
        count = 0;
        if (!folder.ok()) return false;
        std::string manifest;
        for (const std::string &file : files) {
            if (count == limit) break;
            // A file picked again is the same document, as on Android: its clone is not made twice, unless the app
            // removed it or put something else there.
            std::string &name = clones_[(photo ? "p:" : "d:") + file];
            struct stat info {};
            if (name.empty() || ::fstatat(folder.fd(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISREG(info.st_mode)) {
                name.clear();
                std::string content;
                if (!read_file(file, content)) return false;
                const std::size_t slash = file.rfind('/');
                const int fd = create_new(folder, plain_name(slash == std::string::npos ? file : file.substr(slash + 1)),
                                          name);
                if (fd < 0) return false;
                const bool written = write_all(fd, content.data(), content.size());
                ::close(fd);
                if (!written) {
                    name.clear();
                    return false;
                }
            }
            manifest += (photo ? photo_line(file, name) : name) + "\n";
            ++count;
        }
        return write_manifest(id, manifest);
    }

    /// One line of the photo manifest for `file` cloned as `name`: the name, the width and height, the duration (0),
    /// the time of the file in milliseconds and the orientation (0), tab-separated (guest_interface.h).
    static std::string photo_line(const std::string &file, const std::string &name) {
        std::string content;
        std::uint32_t width = 0, height = 0;
        if (read_file(file, content)) picture_size(content, width, height);
        struct stat info {};
        std::int64_t taken = 0;
        if (::stat(file.c_str(), &info) == 0) {
            taken = static_cast<std::int64_t>(info.st_mtimespec.tv_sec) * 1000 + info.st_mtimespec.tv_nsec / 1000000;
        }
        return name + "\t" + std::to_string(width) + "\t" + std::to_string(height) + "\t0\t" + std::to_string(taken) +
               "\t0";
    }

    /// Writes the manifest of request `id` to "<id>.partial" in <data>/.minato/picks and renames it, so that the guest
    /// never reads half of it. A planted file or link of that name is unlinked first, not followed or written to.
    bool write_manifest(std::uint64_t id, const std::string &manifest) {
        const Dir data = open_base(config_.data);
        const Dir minato = open_sub(data, ".minato");
        const Dir picks = open_sub(minato, "picks");
        if (!picks.ok()) return false;
        const std::string target = std::to_string(id);
        const std::string partial = target + ".partial";
        int fd = ::openat(picks.fd(), partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0 && errno == EEXIST) {
            ::unlinkat(picks.fd(), partial.c_str(), 0);
            fd = ::openat(picks.fd(), partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        }
        if (fd < 0) return false;
        const bool written = write_all(fd, manifest.data(), manifest.size()) && ::fsync(fd) == 0;
        ::close(fd);
        if (!written || ::renameat(picks.fd(), partial.c_str(), picks.fd(), target.c_str()) != 0) {
            ::unlinkat(picks.fd(), partial.c_str(), 0);
            return false;
        }
        return true;
    }
};

}  // namespace pick_host
