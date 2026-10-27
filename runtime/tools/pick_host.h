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

#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
    /// The host's own folder for what it must not lose (write-back records and the app's version in a conflict): never
    /// below the data folder or Download, which the app can write. "" keeps records in memory only.
    std::string state;
    /// The most bytes one session exports to Photos in all (ADR-0057 step e); each file also has a size limit.
    std::uint64_t export_bytes = std::uint64_t{4} << 30;
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
inline int create_new(const Dir &dir, const std::string &name, std::string &chosen,
                      const std::function<bool(const std::string &)> &busy = {}) {
    for (int n = 0; n < 10000; ++n) {
        chosen = numbered(name, n);
        if (busy && busy(chosen)) continue;  // a name that something of the host's still holds
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

// ---- Writing an edited clone back to its original (ADR-0057, the addendum accepted 2026-10-23) --------------------
// What Android does at once (the app writes the original through its grant), the host does for a clone: one second
// after the last close of a descriptor the app wrote through, at the session going to the background, and at its end.
// The original is reached through `Original`: on the Mac a path (PathOriginal), on the iPhone a security-scoped
// bookmark and NSFileCoordinator (App). Nothing is written over an original that is not what the clone was made
// from: that is a conflict, the app's version is kept in the host's own folder, and the user decides (resolve).

/// A file's state as the write-back compares it: whether it is there, its size, modification time and SHA-256.
struct Version {
    bool exists = false;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    /// Which file it is: device, inode and birth time of the opened file (0 when not known). A clone is only written
    /// back while this is the file the clone was made as; another file of its name is not the clone.
    std::uint64_t dev = 0, ino = 0;
    std::int64_t birth_ns = 0;
    std::string digest;
    bool same_file(const Version &other) const {
        return dev == other.dev && ino == other.ino && birth_ns == other.birth_ns;
    }
    /// The same content; a changed time alone is no change.
    bool same_content(const Version &other) const {
        return exists == other.exists && size == other.size && digest == other.digest;
    }
};

inline std::string sha256_hex(const std::string &content) {
    unsigned char sum[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(content.data(), static_cast<CC_LONG>(content.size()), sum);
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (const unsigned char byte : sum) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}

inline std::int64_t mtime_ns_of(const struct stat &info) {
    return static_cast<std::int64_t>(info.st_mtimespec.tv_sec) * 1000000000 + info.st_mtimespec.tv_nsec;
}

inline std::int64_t birth_ns_of(const struct stat &info) {
    return static_cast<std::int64_t>(info.st_birthtimespec.tv_sec) * 1000000000 + info.st_birthtimespec.tv_nsec;
}

inline Version version_of(const std::string &content, const struct stat &info) {
    Version version;
    version.exists = true;
    version.size = content.size();
    version.mtime_ns = mtime_ns_of(info);
    version.dev = static_cast<std::uint64_t>(info.st_dev);
    version.ino = info.st_ino;
    version.birth_ns = birth_ns_of(info);
    version.digest = sha256_hex(content);
    return version;
}

/// Reads a regular file below `dir` without following a link at its name.
inline bool read_regular_at(const Dir &dir, const std::string &name, std::string &content, Version &version,
                            int *error = nullptr) {
    // O_NONBLOCK: a FIFO the app made in place of the file must not hold the host (an open would wait for a writer).
    const int fd = ::openat(dir.fd(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (error != nullptr) *error = errno;
        return false;
    }
    struct stat info {};
    bool ok = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
    if (!ok && error != nullptr) *error = EINVAL;
    content.clear();
    char buffer[65536];
    while (ok) {
        const ssize_t got = ::read(fd, buffer, sizeof buffer);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) ok = false;
        if (got <= 0) break;
        content.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(fd);
    if (ok) version = version_of(content, info);
    return ok;
}

/// The user's file a clone was made from, reached the way the host can reach it.
class Original {
public:
    enum Result { WRITTEN, CHANGED, FAILED };
    virtual ~Original() = default;
    /// What the host stores to find the file again (a path here, bookmark data on the iPhone); no tab or newline.
    virtual std::string token() const = 0;
    /// The content now; false when the file is gone, a link, no regular file or cannot be reached.
    virtual bool read(std::string &content, Version &version) = 0;
    /// Replaces the content with `content` if the file still has `expected`'s content, all or nothing: WRITTEN, CHANGED
    /// (it differs, is gone or is a link: nothing was written) or FAILED (it could not be tried).
    virtual Result replace_if(const Version &expected, const std::string &content, Version &after) = 0;
};

/// An original by path (apk-run on the Mac): never read or written through a link at its name.
class PathOriginal : public Original {
public:
    explicit PathOriginal(std::string path) : path_(std::move(path)) {}
    std::string token() const override { return path_; }
    bool read(std::string &content, Version &version) override {
        const std::size_t slash = path_.rfind('/');
        const Dir dir = open_base(slash == std::string::npos ? "." : slash == 0 ? "/" : path_.substr(0, slash));
        return dir.ok() &&
               read_regular_at(dir, path_.substr(slash == std::string::npos ? 0 : slash + 1), content, version);
    }
    Result replace_if(const Version &expected, const std::string &content, Version &after) override {
        std::string old;
        Version now;
        if (!read(old, now) || !now.same_content(expected)) return CHANGED;
        const std::size_t slash = path_.rfind('/');
        const Dir dir = open_base(slash == std::string::npos ? "." : slash == 0 ? "/" : path_.substr(0, slash));
        const std::string name = path_.substr(slash == std::string::npos ? 0 : slash + 1);
        const std::string partial = "." + name + ".minato-partial";
        ::unlinkat(dir.fd(), partial.c_str(), 0);
        const int fd = ::openat(dir.fd(), partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0) return FAILED;
        const bool written = write_all(fd, content.data(), content.size()) && ::fsync(fd) == 0;
        ::close(fd);
        if (!written || ::renameat(dir.fd(), partial.c_str(), dir.fd(), name.c_str()) != 0) {
            ::unlinkat(dir.fd(), partial.c_str(), 0);
            return FAILED;
        }
        std::string back;
        return read(back, after) ? WRITTEN : FAILED;
    }

private:
    std::string path_;
};

/// The records of the clones the host made from the user's files, and what becomes of an edit.
class WriteBack {
public:
    /// DISCARDED was a state of the first draft; a record the user lets go, or whose clone is gone, is forgotten now.
    enum State { CLEAN, DIRTY, CONFLICT };
    /// The wait after the last close, as the addendum says: an app that closes and opens again writes once.
    static constexpr std::int64_t delay_ms = 1000;
    static constexpr int max_failures = 6;
    /// Called, with no lock held, with the name of each clone whose record the host let go (the host stops watching
    /// it).
    std::function<void(const std::string &)> on_forget;
    using Factory = std::function<std::unique_ptr<Original>(const std::string &token)>;

    WriteBack(std::string download, std::string state) : download_(std::move(download)), state_(std::move(state)) {}

    /// Starts watching `clone` (a name in Download), made from `original`, which had `original_now`; `clone_now` is the
    /// clone as it was made.
    void track(const std::string &clone, std::unique_ptr<Original> original, const Version &original_now,
               const Version &clone_now) {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        Record record;
        record.original = std::move(original);
        record.original_base = original_now;
        record.clone_base = clone_now;
        // The clone's own identity, as it is in the folder right after it was made: whatever the caller's version says.
        record.clone_base.dev = record.clone_base.ino = 0;
        record.clone_base.birth_ns = 0;
        const Dir download = open_base(download_);
        struct stat info {};
        if (download.ok() && ::fstatat(download.fd(), clone.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(info.st_mode)) {
            record.clone_base.dev = static_cast<std::uint64_t>(info.st_dev);
            record.clone_base.ino = info.st_ino;
            record.clone_base.birth_ns = birth_ns_of(info);
        }
        records_[clone] = std::move(record);
        save();
    }
    bool tracked(const std::string &clone) const {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        return records_.count(clone) != 0;
    }
    /// Whether a record holds `clone` with the app's work in it (DIRTY or CONFLICT): the name is not given again, even
    /// when the guest deleted the clone, since the record's kept copy and its original are still the user's to decide.
    bool holds(const std::string &clone) const {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto found = records_.find(clone);
        return found != records_.end() && found->second.state != CLEAN;
    }
    /// Lets the record of an untouched clone go (its file is gone and the name is taken by another).
    void forget_clean(const std::string &clone) {
        announce([&] {
            const auto found = records_.find(clone);
            if (found != records_.end() && found->second.state == CLEAN) forget(clone);
        });
    }

    /// Whether a file picked again may get `clone` again. Android's document is live, so a clone the app has not
    /// touched (CLEAN) is only reused while the original still has the content it was made from; the user changed
    /// it elsewhere otherwise, and the pick gets a new clone. A clone with the app's edits (DIRTY, CONFLICT) stays: the
    /// write-back decides what becomes of the original. An original that cannot be read leaves the clone as it is. A
    /// clone with no record (the user let its conflict go, so it holds a rejected version) is not reused.
    bool reusable(const std::string &clone) {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto found = records_.find(clone);
        if (found == records_.end()) return false;  // a clone without a record is one the host let go of
        if (found->second.state != CLEAN) return true;
        std::string content;
        Version now;
        return !found->second.original->read(content, now) || now.same_content(found->second.original_base);
    }

    /// The app closed a descriptor it wrote through.
    void closed(const std::string &clone, std::int64_t now_ms) {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto found = records_.find(clone);
        if (found == records_.end()) return;
        Record &record = found->second;
        if (record.state == CONFLICT) return;
        record.state = DIRTY;
        record.settled = true;
        record.missed = 0;  // a count of the clean clone's scans says nothing of the write that has just ended
        record.due_ms = now_ms + delay_ms;
        save();
    }

    /// Finds the clones that changed without a close being reported (a write through a mapping, a rename over the
    /// clone); they are written back one second later, as after a close. A clone that is untouched and gone at
    /// two scans in a row is forgotten: a file the app makes under that name later is its own, not the user's file.
    void scan(std::int64_t now_ms) {
        announce([&] { scan_locked(now_ms, false); });
    }

    /// Writes back what is due at `now_ms`, or everything dirty when `force` (the session goes to the background).
    void flush(std::int64_t now_ms, bool force) {
        announce([&] { flush_locked(now_ms, force, false); });
    }

    /// The session ends: what a mapping changed is found, everything is written, the records are stored.
    void end_session(std::int64_t now_ms) {
        announce([&] {
            scan_locked(now_ms, true);
            flush_locked(now_ms, true, true);
            save();
        });
    }

    /// Whether the guest may delete the clone: it is written back, or never changed, or the user let it go.
    bool removable(const std::string &clone) const {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto found = records_.find(clone);
        return found == records_.end() || found->second.state == CLEAN;
    }

    std::vector<std::string> conflicts() const {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::vector<std::string> names;
        for (const auto &entry : records_) {
            if (entry.second.state == CONFLICT) names.push_back(entry.first);
        }
        return names;
    }

    /// The user's decision for a conflict: `replace` writes the app's version over what the original is now, else the
    /// app's version is let go and the record is forgotten (the clone is an ordinary file of the app from then on).
    /// False when there is no such conflict or the write did not happen.
    bool resolve(const std::string &clone, bool replace) {
        bool done = false;
        announce([&] { done = resolve_locked(clone, replace); });
        return done;
    }

    /// Stores the records in the host's own folder (not a folder the app can write).
    bool save() const {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (state_.empty()) return true;
        const Dir state = open_base(state_);
        if (!state.ok()) return false;
        std::string text;
        for (const auto &entry : records_) {
            const Record &r = entry.second;
            const std::string token = r.original->token();
            if (token.find_first_of("\t\n") != std::string::npos) continue;
            text += entry.first + "\t" + static_cast<char>('0' + r.state) + "\t" + token + "\t" +
                    fields(r.original_base) + "\t" + fields(r.clone_base) + "\t" + r.copy + "\t" +
                    std::to_string(r.due_ms) + "\n";
        }
        const std::string partial = "records.partial";
        ::unlinkat(state.fd(), partial.c_str(), 0);
        const int fd =
            ::openat(state.fd(), partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return false;
        const bool written = write_all(fd, text.data(), text.size()) && ::fsync(fd) == 0;
        ::close(fd);
        if (!written || ::renameat(state.fd(), partial.c_str(), state.fd(), "records") != 0) {
            ::unlinkat(state.fd(), partial.c_str(), 0);
            return false;
        }
        return true;
    }

    /// Reads the records stored by save(), each original made by `factory`; false when there are none.
    bool load(const Factory &factory) {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (state_.empty()) return false;
        const Dir state = open_base(state_);
        std::string text;
        Version ignored;
        if (!state.ok() || !read_regular_at(state, "records", text, ignored)) return false;
        for (std::size_t start = 0; start < text.size();) {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::vector<std::string> field;
            const std::string line = text.substr(start, end - start);
            start = end + 1;
            for (std::size_t from = 0;; ) {
                const std::size_t tab = line.find('\t', from);
                field.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
                if (tab == std::string::npos) break;
                from = tab + 1;
            }
            if (field.size() != 7 || field[0].empty() || field[0].find('/') != std::string::npos ||
                field[1].size() != 1 || field[1][0] < '0' || field[1][0] > '2' ||
                field[5].find('/') != std::string::npos) {
                continue;
            }
            Record record;
            record.original = factory(field[2]);
            if (!record.original || !parse_fields(field[3], record.original_base) ||
                !parse_fields(field[4], record.clone_base)) {
                continue;
            }
            record.state = static_cast<State>(field[1][0] - '0');
            record.settled = true;  // the process that wrote it has ended
            record.copy = field[5];
            record.due_ms = std::atoll(field[6].c_str());
            records_[field[0]] = std::move(record);
        }
        return true;
    }

private:
    bool resolve_locked(const std::string &clone, bool replace) {
        const auto found = records_.find(clone);
        if (found == records_.end() || found->second.state != CONFLICT) return false;
        Record &record = found->second;
        if (replace) {
            std::string content;
            Version copy;
            const Dir state = open_base(state_);
            if (record.copy.empty() || !state.ok() || !read_regular_at(state, record.copy, content, copy)) return false;
            std::string old;
            Version now;
            Version after;
            if (!record.original->read(old, now) ||
                record.original->replace_if(now, content, after) != Original::WRITTEN) {
                return false;
            }
            record.original_base = after;
            copy.dev = record.clone_base.dev;                                // the clone's own identity stays
            copy.ino = record.clone_base.ino;
            copy.birth_ns = record.clone_base.birth_ns;
            record.clone_base = copy;
            record.state = CLEAN;
            drop_copy(record);
        } else {
            forget(clone);
        }
        save();
        return true;
    }

    void scan_locked(std::int64_t now_ms, bool final) {
        bool changed = false;
        const Dir download = open_base(download_);
        std::vector<std::string> gone;
        for (auto &entry : records_) {
            Record &record = entry.second;
            if (record.state == CONFLICT) continue;
            struct stat info {};
            if (!download.ok()) continue;
            if (record.state == DIRTY) {
                // A write that goes on (a second burst through a descriptor opened again, a mapping) pushes the write
                // back: the clone is copied when size and time have stood still for the delay.
                if (::fstatat(download.fd(), entry.first.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) continue;
                const std::uint64_t size = static_cast<std::uint64_t>(info.st_size);
                const std::int64_t mtime = mtime_ns_of(info);
                if (record.seen_known && (size != record.seen_size || mtime != record.seen_mtime_ns)) {
                    record.due_ms = now_ms + delay_ms;
                }
                record.seen_known = true;
                record.seen_size = size;
                record.seen_mtime_ns = mtime;
                continue;
            }
            if (::fstatat(download.fd(), entry.first.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
                // Missing once may be a save that unlinks the file before it renames another over it: the record
                // waits for the next scan, which finds that file (another identity: a conflict) or still nothing.
                if (errno == ENOENT && (final || ++record.missed >= 2)) gone.push_back(entry.first);
                continue;
            }
            record.missed = 0;
            Version now;
            now.dev = static_cast<std::uint64_t>(info.st_dev);
            now.ino = info.st_ino;
            now.birth_ns = birth_ns_of(info);
            if (!S_ISREG(info.st_mode) || static_cast<std::uint64_t>(info.st_size) != record.clone_base.size ||
                mtime_ns_of(info) != record.clone_base.mtime_ns || !now.same_file(record.clone_base)) {
                record.state = DIRTY;
                // Another file under the name is no write in progress of a watched descriptor: it settles by
                // standing still (the scans above), as nothing reports its writers.
                record.settled = S_ISREG(info.st_mode) && !now.same_file(record.clone_base);
                record.seen_known = true;
                record.seen_size = static_cast<std::uint64_t>(info.st_size);
                record.seen_mtime_ns = mtime_ns_of(info);
                record.due_ms = now_ms + delay_ms;
                changed = true;
            }
        }
        for (const std::string &name : gone) forget(name);
        if (changed || !gone.empty()) save();
    }

    void flush_locked(std::int64_t now_ms, bool force, bool final) {
        std::vector<std::string> due;
        for (const auto &entry : records_) {
            // A change no close was reported for waits, whatever `force` says, until the session's end: a writer may
            // still hold the clone then, and a copy of it in mid-write would replace the user's file.
            if (entry.second.state != DIRTY || !(entry.second.settled || final)) continue;
            if (force || entry.second.due_ms <= now_ms) due.push_back(entry.first);
        }
        for (const std::string &name : due) {
            write_back(name, records_.at(name), now_ms, final);
            save();
        }
    }

    /// Runs `body` under the lock; the clones it forgot are announced after the lock is gone, since the host's answer
    /// (it stops watching, which waits for a close that may be waiting for this lock) must not run under it.
    template <typename Body>
    void announce(Body body) {
        std::vector<std::string> gone;
        {
            const std::lock_guard<std::recursive_mutex> lock(mutex_);
            body();
            gone.swap(forgotten_);
        }
        if (on_forget) {
            for (const std::string &name : gone) on_forget(name);
        }
    }

    /// Lets the record of `clone` go: its stored app version, too.
    void forget(const std::string &clone) {
        const auto found = records_.find(clone);
        if (found == records_.end()) return;
        drop_copy(found->second);
        records_.erase(found);
        forgotten_.push_back(clone);
    }

    struct Record {
        std::unique_ptr<Original> original;
        Version original_base, clone_base;
        State state = CLEAN;
        std::int64_t due_ms = 0;
        int failures = 0;  // of the write in a row; not stored
        int missed = 0;    // scans or flushes in a row that found the clone missing; not stored
        // Whether the watch reported the last writer's close (or the file is another one, or an earlier run left
        // the record): only then may the clone be copied. A change a scan found alone may be an open descriptor's
        // write in progress, so it waits for the close or the session's end. Not stored: a loaded record is settled.
        bool settled = false;
        // The size and time the last scan saw of a changed clone; while they change, the write is pushed back.
        bool seen_known = false;
        std::uint64_t seen_size = 0;
        std::int64_t seen_mtime_ns = 0;
        std::string copy;
    };

    static std::string fields(const Version &v) {
        return std::string(v.exists ? "1" : "0") + "," + std::to_string(v.size) + "," + std::to_string(v.mtime_ns) +
               "," + std::to_string(v.dev) + "," + std::to_string(v.ino) + "," + std::to_string(v.birth_ns) + "," +
               v.digest;
    }
    static bool parse_fields(const std::string &text, Version &v) {
        std::size_t at[6];
        std::size_t from = 0;
        for (std::size_t &comma : at) {
            comma = text.find(',', from);
            if (comma == std::string::npos) return false;
            from = comma + 1;
        }
        if (text[0] != '0' && text[0] != '1') return false;
        v.exists = text[0] == '1';
        v.size = std::strtoull(text.substr(at[0] + 1, at[1] - at[0] - 1).c_str(), nullptr, 10);
        v.mtime_ns = std::atoll(text.substr(at[1] + 1, at[2] - at[1] - 1).c_str());
        v.dev = std::strtoull(text.substr(at[2] + 1, at[3] - at[2] - 1).c_str(), nullptr, 10);
        v.ino = std::strtoull(text.substr(at[3] + 1, at[4] - at[3] - 1).c_str(), nullptr, 10);
        v.birth_ns = std::atoll(text.substr(at[4] + 1, at[5] - at[4] - 1).c_str());
        v.digest = text.substr(at[5] + 1);
        return v.digest.size() == 64 || v.digest.empty();
    }

    void drop_copy(Record &record) {
        if (!record.copy.empty()) {
            const Dir state = open_base(state_);
            if (state.ok()) ::unlinkat(state.fd(), record.copy.c_str(), 0);
        }
        record.copy.clear();
    }

    /// Keeps the app's version in the host's folder: the guest's cleaning may delete the clone in Download.
    void keep_copy(Record &record, const std::string &content) {
        if (state_.empty()) return;
        const Dir state = open_base(state_);
        if (!state.ok()) return;
        std::string chosen;
        const int fd = create_new(state, "copy-" + std::to_string(++sequence_), chosen);
        if (fd < 0) return;
        const bool written = write_all(fd, content.data(), content.size()) && ::fsync(fd) == 0;
        ::close(fd);
        if (written) record.copy = chosen; else ::unlinkat(state.fd(), chosen.c_str(), 0);
    }

    void write_back(const std::string &clone, Record &record, std::int64_t now_ms, bool final) {
        const Dir download = open_base(download_);
        std::string content;
        Version version;
        int error = 0;
        if (!download.ok() || !read_regular_at(download, clone, content, version, &error)) {
            if (error == ENOENT) {
                if (final || ++record.missed >= 2) {
                    forget(clone);                                           // nothing to write back, nothing to guard
                } else {
                    record.due_ms = now_ms + delay_ms;                       // a save in between may bring the file
                }
            } else {
                record.state = CONFLICT;
            }
            return;
        }
        record.missed = 0;
        if (!version.same_file(record.clone_base)) {
            // Another file has this name now (the app deleted the clone and made one, or renamed one over it): it is
            // not what was cloned, so it is never written over the user's file; the user decides.
            keep_copy(record, content);
            record.state = CONFLICT;
            return;
        }
        if (version.same_content(record.clone_base)) {
            record.clone_base = version;
            record.state = CLEAN;
            return;
        }
        Version after;
        switch (record.original->replace_if(record.original_base, content, after)) {
            case Original::WRITTEN:
                record.original_base = after;
                record.clone_base = version;
                record.state = CLEAN;
                break;
            case Original::CHANGED:
                keep_copy(record, content);
                record.state = CONFLICT;
                break;
            case Original::FAILED:
                // Tried again after 1, 2, 4, 8 and 16 s; the sixth failure asks the user instead.
                if (++record.failures >= max_failures) {
                    keep_copy(record, content);
                    record.state = CONFLICT;
                } else {
                    record.due_ms = now_ms + (delay_ms << (record.failures - 1));
                }
                break;
        }
    }

    mutable std::recursive_mutex mutex_;
    std::string download_, state_;
    std::map<std::string, Record> records_;
    unsigned sequence_ = 0;
    std::vector<std::string> forgotten_;
};

/// A file taken for the export to Photos (Host::export_begin): its descriptor, kept until the copy is made. On the
/// iPhone the host gives Photos only a copy it made in a folder of its own from this descriptor, never the guest's
/// path, which the app could swap for a link meanwhile.
class ExportJob {
public:
    ExportJob() = default;
    ExportJob(int source, std::string name, off_t size) : source_(source), name_(std::move(name)), size_(size) {}
    ExportJob(ExportJob &&other) noexcept
        : source_(other.source_), name_(std::move(other.name_)), size_(other.size_) {
        other.source_ = -1;
    }
    ExportJob &operator=(ExportJob &&other) noexcept {
        if (this != &other) {
            reset();
            source_ = other.source_;
            name_ = std::move(other.name_);
            size_ = other.size_;
            other.source_ = -1;
        }
        return *this;
    }
    ExportJob(const ExportJob &) = delete;
    ExportJob &operator=(const ExportJob &) = delete;
    ~ExportJob() { reset(); }

    bool ok() const { return source_ >= 0; }

    /// Copies what the file had when it was taken (no more than its size then) into the folder `to`, under its name
    /// made unique; a copy that fails leaves nothing. Ends the job.
    bool run(const std::string &to) {
        if (source_ < 0) return false;
        const Dir destination = open_base(to);
        std::string chosen;
        const int fd = destination.ok() ? create_new(destination, plain_name(name_), chosen) : -1;
        bool copied = fd >= 0;
        char buffer[65536];
        for (off_t left = size_; copied && left > 0;) {
            const ssize_t got = ::read(source_, buffer, static_cast<std::size_t>(std::min<off_t>(left, sizeof buffer)));
            if (got < 0 && errno == EINTR) continue;
            copied = got > 0 && write_all(fd, buffer, static_cast<std::size_t>(got));
            left -= got;
        }
        if (fd >= 0) ::close(fd);
        if (!copied && fd >= 0) ::unlinkat(destination.fd(), chosen.c_str(), 0);  // no half a file in Photos
        reset();
        return copied;
    }

private:
    void reset() {
        if (source_ >= 0) ::close(source_);
        source_ = -1;
    }
    int source_ = -1;
    std::string name_;
    off_t size_ = 0;
};

class Host {
public:
    explicit Host(Config config)
        : config_(std::move(config)), write_back_(config_.download, config_.state) {}

    /// The most files one session exports, the largest one, and (Config::export_bytes) the most bytes in all: an app
    /// must not fill the storage or the library.
    static constexpr int export_count_limit = 1000;
    static constexpr off_t export_size_limit = off_t{1} << 30;

    /// APKRT_REQUEST_EXPORT_MEDIA (ADR-0057 step e), the first part, which the request handler does at once: the file
    /// at the text's path ("<kind>\t<path>" below the data folder) is taken by a descriptor, walked from the data
    /// folder's descriptor with O_NOFOLLOW at every name, opened without blocking, a regular file with one link
    /// within the limits.
    /// The job then copies from that descriptor, never from the path again, when the handler is done (run).
    ExportJob export_begin(const std::string &text) {
        if (text.size() < 3 || (text[0] != '1' && text[0] != '2') || text[1] != '\t') return {};
        std::vector<std::string> parts;
        for (std::size_t from = 2;;) {
            const std::size_t slash = text.find('/', from);
            parts.push_back(text.substr(from, slash == std::string::npos ? std::string::npos : slash - from));
            if (slash == std::string::npos) break;
            from = slash + 1;
        }
        for (const std::string &part : parts) {
            if (part.empty() || part == "." || part == ".." || parts.size() > 16) return {};
        }
        Dir dir = open_base(config_.data);
        for (std::size_t i = 0; dir.ok() && i + 1 < parts.size(); ++i) {
            dir = Dir(::openat(dir.fd(), parts[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        }
        if (!dir.ok() || exports_ >= export_count_limit) return {};
        // A file the app made can be a FIFO, endless or huge: opened without blocking, it must be a regular file.
        const int source = ::openat(dir.fd(), parts.back().c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (source < 0) return {};
        struct stat info {};
        if (::fstat(source, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
            info.st_size > export_size_limit ||
            export_bytes_ + static_cast<std::uint64_t>(info.st_size) > config_.export_bytes) {
            ::close(source);
            return {};
        }
        ++exports_;
        export_bytes_ += static_cast<std::uint64_t>(info.st_size);
        return ExportJob(source, parts.back(), info.st_size);
    }

    /// Both parts at once, for a caller that has no thread to spare: the file is copied into the folder `to`.
    bool export_media(const std::string &text, const std::string &to) {
        ExportJob job = export_begin(text);
        return job.ok() && job.run(to);
    }

    /// The records of the clones made from the user's files (kinds 1 and 2).
    WriteBack &write_back() { return write_back_; }
    /// The files the next answers pick (apk-run's model takes them from its command line once; a test changes them).
    void set_files(std::vector<std::string> files) { config_.files = std::move(files); }

    /// Called with the name of each clone this host makes from a user's file (not for one picked again): the place to
    /// ask the runtime to watch it (apkrt_watch_writes), so that write_back().closed() hears the app's last close.
    std::function<void(const std::string &clone)> on_clone;

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
    WriteBack write_back_;
    int exports_ = 0;
    std::uint64_t export_bytes_ = 0;
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
                !S_ISREG(info.st_mode) || (!photo && !write_back_.reusable(name))) {
                name.clear();
                std::string content;
                if (!read_file(file, content)) return false;
                const std::size_t slash = file.rfind('/');
                const std::string base = slash == std::string::npos ? file : file.substr(slash + 1);
                // A name that a record still holds (a conflict whose clone the guest deleted) is not taken again.
                const int fd = create_new(folder, plain_name(base), name,
                                          [&](const std::string &taken) { return !photo && write_back_.holds(taken); });
                if (fd < 0) return false;
                const bool written = write_all(fd, content.data(), content.size()) && ::fsync(fd) == 0;
                struct stat made {};
                const bool stated = written && ::fstat(fd, &made) == 0;
                ::close(fd);
                if (!stated) {
                    name.clear();
                    return false;
                }
                if (!photo) write_back_.forget_clean(name);  // an untouched clone of that name is gone: its record too
                // The name is this document's now: an older entry that names it (its clone was deleted, and this
                // file took the free name) would hand back the wrong file.
                for (auto other = clones_.begin(); other != clones_.end();) {
                    const bool stale = &other->second != &name && other->second == name;
                    other = stale ? clones_.erase(other) : std::next(other);
                }
                struct stat source {};
                if (!photo && ::stat(file.c_str(), &source) == 0) {
                    write_back_.track(name, std::make_unique<PathOriginal>(file), version_of(content, source),
                                      version_of(content, made));
                }
                if (!photo && on_clone) on_clone(name);
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
