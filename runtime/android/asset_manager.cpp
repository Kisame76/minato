// libandroid.so's asset manager (ADR-0028, decision 10), written for Minato from the NDK's documentation and
// Android 15's answers (fixtures/apps/activity-test), with the ZIP format's own rules. activity-host makes the
// one AAssetManager of the app with minato_AAssetManager_create, which reads the central directory of the
// app's base.apk once, through the guest's own file access, and keeps its entries below assets/, sorted by
// name. Of an installed set it does so for each split too, and a name is looked up in the last APK first (ADR-0048
// decision 9). A central directory that is not sound (no end record exactly at the end, a split or ZIP64 archive,
// headers that run past it, two entries of one name) makes the manager fail. An entry is checked when it is
// opened: stored or deflated, not encrypted, no ZIP64 sizes, a local header of the same name, and data that
// ends before the central directory; a broken entry fails only its own open. Stored entries are read in
// place: AAsset_read reads the APK, AAsset_getBuffer maps its pages, and AAsset_openFileDescriptor gives a
// new descriptor of the APK with the entry's offset and length. Deflated entries are inflated with libz as
// they are read, again from the beginning for a position before the stream's, and whole only for
// AAsset_getBuffer; data that inflates to less or more than its size is broken. Every buffer is on the heap,
// since an app's thread may have a small stack. AAssetDir lists the files directly in a folder, not its
// folders, sorted by name. Names are looked up exactly, as Android does: "a/../b" is not "b". Problems go to
// the log; a missing asset does not.
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <zlib.h>

#include "APKRuntime/guest_interface.h"
#include "internal.h"

namespace {

constexpr uint32_t kEndSignature = 0x06054b50, kCentralSignature = 0x02014b50, kLocalSignature = 0x04034b50;
constexpr size_t kEndSize = 22, kCentralSize = 46, kLocalSize = 30, kMaxComment = 0xffff;
constexpr char kPrefix[] = "assets/";
constexpr size_t kPrefixSize = sizeof kPrefix - 1;

struct Entry {
    const char *name;  // the whole name, in the manager's copy of the central directory, not NUL-terminated
    uint32_t name_size;
    uint16_t flags, method;
    uint32_t compressed, uncompressed, local_header;
};

uint16_t get16(const unsigned char *at) {
    return static_cast<uint16_t>(at[0] | at[1] << 8);
}

uint32_t get32(const unsigned char *at) {
    return static_cast<uint32_t>(at[0]) | static_cast<uint32_t>(at[1]) << 8 | static_cast<uint32_t>(at[2]) << 16 |
           static_cast<uint32_t>(at[3]) << 24;
}

// Reads exactly `size` bytes at `offset`.
bool read_at(int fd, void *buffer, size_t size, uint64_t offset) {
    auto *bytes = static_cast<unsigned char *>(buffer);
    while (size > 0) {
        const ssize_t got = pread64(fd, bytes, size, static_cast<off64_t>(offset));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        bytes += got;
        size -= static_cast<size_t>(got);
        offset += static_cast<uint64_t>(got);
    }
    return true;
}

// Orders names as strcmp orders C strings.
int compare(const char *a, size_t a_size, const char *b, size_t b_size) {
    const int order = memcmp(a, b, a_size < b_size ? a_size : b_size);
    if (order != 0) return order;
    return a_size < b_size ? -1 : a_size > b_size ? 1 : 0;
}

int compare_entries(const void *a, const void *b) {
    const auto *left = static_cast<const Entry *>(a);
    const auto *right = static_cast<const Entry *>(b);
    return compare(left->name, left->name_size, right->name, right->name_size);
}

}  // namespace

struct AAssetManager {
    int fd;                   // the APK, read with pread
    char *apk;                // its guest path, for AAsset_openFileDescriptor
    uint64_t central_offset;  // an entry's data ends before it
    unsigned char *directory;
    Entry *entries;  // below assets/, sorted by name
    size_t count;
    AConfiguration *configuration;
    AAssetManager *earlier;  // the APK before this one in the set: a name this one lacks is looked up there
};

// Inflating a deflated asset while it is read, on the heap, since an app's thread may have a small stack.
struct Inflater {
    z_stream stream;
    uint64_t consumed;  // compressed bytes read into the stream
    uint64_t produced;  // uncompressed bytes out of it: the position it has reached
    bool ended;         // the stream said its end
    unsigned char input[32768];
    unsigned char passed[16384];  // where the bytes before a later position go
};

struct AAsset {
    AAssetManager *manager;
    const Entry *entry;
    uint64_t data;  // where the entry's data starts in the APK
    uint64_t length;
    uint64_t position;
    Inflater *inflater;       // a deflated asset's, from its first read
    unsigned char *contents;  // a deflated asset's whole, once AAsset_getBuffer asked for it
    void *mapping;            // a stored asset's pages of the APK, once AAsset_getBuffer asked for them
    size_t mapping_size;
    const unsigned char *mapped;
    bool broken;  // its deflated data failed once, and fails again
};

struct AAssetDir {
    char **names;  // in one allocation with the names themselves
    size_t count;
    size_t next;
};

namespace {

void warn(const AAssetManager *manager, const Entry *entry, const char *problem) {
    if (entry == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, "libandroid", "%s: %s", manager->apk, problem);
    } else {
        __android_log_print(ANDROID_LOG_WARN, "libandroid", "%s: %.*s: %s", manager->apk,
                            static_cast<int>(entry->name_size), entry->name, problem);
    }
}

void destroy(AAssetManager *manager) {
    if (manager->earlier != nullptr) destroy(manager->earlier);
    if (manager->fd >= 0) close(manager->fd);
    if (manager->configuration != nullptr) AConfiguration_delete(manager->configuration);
    free(manager->entries);
    free(manager->directory);
    free(manager->apk);
    free(manager);
}

// The central directory's entries below assets/, or false when the directory is not sound.
bool read_directory(AAssetManager *manager) {
    struct stat status;
    if (fstat(manager->fd, &status) != 0 || !S_ISREG(status.st_mode)) {
        warn(manager, nullptr, "cannot be read");
        return false;
    }
    const uint64_t size = static_cast<uint64_t>(status.st_size);
    if (size < kEndSize) {
        warn(manager, nullptr, "no end record");
        return false;
    }
    // The end record: the last one whose comment ends exactly at the end of the file.
    const size_t tail_size = static_cast<size_t>(size < kEndSize + kMaxComment ? size : kEndSize + kMaxComment);
    auto *tail = static_cast<unsigned char *>(malloc(tail_size));
    if (tail == nullptr || !read_at(manager->fd, tail, tail_size, size - tail_size)) {
        free(tail);
        warn(manager, nullptr, "cannot be read");
        return false;
    }
    const unsigned char *end = nullptr;
    for (size_t at = tail_size - kEndSize + 1; at-- > 0;) {
        if (get32(tail + at) == kEndSignature && at + kEndSize + get16(tail + at + 20) == tail_size) {
            end = tail + at;
            break;
        }
    }
    if (end == nullptr) {
        free(tail);
        warn(manager, nullptr, "no end record");
        return false;
    }
    const uint64_t end_offset = size - tail_size + static_cast<uint64_t>(end - tail);
    const uint16_t disk = get16(end + 4), central_disk = get16(end + 6), disk_entries = get16(end + 8);
    const uint16_t entries = get16(end + 10);
    const uint32_t central_size = get32(end + 12), central_offset = get32(end + 16);
    free(tail);
    if (disk != 0 || central_disk != 0 || disk_entries != entries || entries == 0xffff || central_size == 0xffffffff ||
        central_offset == 0xffffffff) {
        warn(manager, nullptr, "a split or ZIP64 archive");
        return false;
    }
    if (static_cast<uint64_t>(central_offset) + central_size > end_offset) {
        warn(manager, nullptr, "the central directory runs into the end record");
        return false;
    }
    manager->central_offset = central_offset;
    manager->directory = static_cast<unsigned char *>(malloc(central_size > 0 ? central_size : 1));
    manager->entries = static_cast<Entry *>(malloc(entries > 0 ? entries * sizeof(Entry) : sizeof(Entry)));
    if (manager->directory == nullptr || manager->entries == nullptr ||
        !read_at(manager->fd, manager->directory, central_size, central_offset)) {
        warn(manager, nullptr, "cannot be read");
        return false;
    }
    size_t offset = 0;
    for (uint32_t index = 0; index < entries; ++index) {
        const unsigned char *header = manager->directory + offset;
        if (central_size - offset < kCentralSize || get32(header) != kCentralSignature) {
            warn(manager, nullptr, "a broken central directory");
            return false;
        }
        const size_t name_size = get16(header + 28);
        const size_t next = offset + kCentralSize + name_size + get16(header + 30) + get16(header + 32);
        if (next > central_size) {
            warn(manager, nullptr, "a broken central directory");
            return false;
        }
        const char *name = reinterpret_cast<const char *>(header + kCentralSize);
        // Only files below assets/: no folder entries, and no names a C string cannot ask for.
        if (name_size > kPrefixSize && memcmp(name, kPrefix, kPrefixSize) == 0 && name[name_size - 1] != '/' &&
            memchr(name, 0, name_size) == nullptr) {
            manager->entries[manager->count++] = {name,
                                                  static_cast<uint32_t>(name_size),
                                                  get16(header + 8),
                                                  get16(header + 10),
                                                  get32(header + 20),
                                                  get32(header + 24),
                                                  get32(header + 42)};
        }
        offset = next;
    }
    qsort(manager->entries, manager->count, sizeof(Entry), compare_entries);
    for (size_t index = 1; index < manager->count; ++index) {
        if (compare_entries(&manager->entries[index - 1], &manager->entries[index]) == 0) {
            warn(manager, &manager->entries[index], "two entries of this name");
            return false;
        }
    }
    return true;
}

// The first entry whose name is not below "assets/" + `name` (`size` bytes).
size_t lower_bound(const AAssetManager *manager, const char *name, size_t size) {
    size_t low = 0, high = manager->count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const Entry &entry = manager->entries[middle];
        if (compare(entry.name + kPrefixSize, entry.name_size - kPrefixSize, name, size) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

const Entry *find(const AAssetManager *manager, const char *name) {
    const size_t size = strlen(name);
    const size_t index = lower_bound(manager, name, size);
    if (index == manager->count) return nullptr;
    const Entry &entry = manager->entries[index];
    return compare(entry.name + kPrefixSize, entry.name_size - kPrefixSize, name, size) == 0 ? &entry : nullptr;
}

// Where the entry's data starts, or 0 when the entry cannot be read.
uint64_t data_of(AAssetManager *manager, const Entry *entry) {
    if ((entry->flags & 1) != 0) {
        warn(manager, entry, "encrypted");
        return 0;
    }
    if (entry->method != 0 && entry->method != 8) {
        warn(manager, entry, "neither stored nor deflated");
        return 0;
    }
    if (entry->compressed == 0xffffffff || entry->uncompressed == 0xffffffff) {
        warn(manager, entry, "ZIP64 sizes");
        return 0;
    }
    if (entry->method == 0 && entry->compressed != entry->uncompressed) {
        warn(manager, entry, "stored, but its sizes differ");
        return 0;
    }
    unsigned char header[kLocalSize];
    const uint64_t local = entry->local_header;
    bool sound = local + kLocalSize + entry->name_size <= manager->central_offset &&
                 read_at(manager->fd, header, kLocalSize, local) && get32(header) == kLocalSignature &&
                 get16(header + 26) == entry->name_size;
    // The local header's name, in pieces, since an app's thread may have a small stack.
    char piece[256];
    for (uint32_t done = 0; sound && done < entry->name_size; done += sizeof piece) {
        const uint32_t size = entry->name_size - done < sizeof piece ? entry->name_size - done : sizeof piece;
        sound = read_at(manager->fd, piece, size, local + kLocalSize + done) &&
                memcmp(piece, entry->name + done, size) == 0;
    }
    if (!sound) {
        warn(manager, entry, "a broken local header");
        return 0;
    }
    const uint64_t data = local + kLocalSize + entry->name_size + get16(header + 28);
    if (data + entry->compressed > manager->central_offset) {
        warn(manager, entry, "its data runs into the central directory");
        return 0;
    }
    return data;
}

// Marks the asset broken, which it stays, and says so once.
bool broke(AAsset *asset) {
    if (!asset->broken) warn(asset->manager, asset->entry, "broken deflated data");
    asset->broken = true;
    return false;
}

// Starts inflating the asset from its beginning.
bool restart(AAsset *asset) {
    Inflater *inflater = asset->inflater;
    if (inflater == nullptr) {
        inflater = static_cast<Inflater *>(calloc(1, sizeof(Inflater)));
        if (inflater == nullptr) return false;
        if (inflateInit2(&inflater->stream, -MAX_WBITS) != Z_OK) {
            free(inflater);
            return false;
        }
        asset->inflater = inflater;
    } else if (inflateReset(&inflater->stream) != Z_OK) {
        return false;
    }
    inflater->stream.avail_in = 0;
    inflater->consumed = 0;
    inflater->produced = 0;
    inflater->ended = false;
    return true;
}

// Inflates exactly `size` bytes into `out`; false when the data is broken or ends first.
bool pull(AAsset *asset, unsigned char *out, size_t size) {
    Inflater &inflater = *asset->inflater;
    z_stream &stream = inflater.stream;
    stream.next_out = out;
    stream.avail_out = static_cast<uInt>(size);
    while (stream.avail_out > 0) {
        if (inflater.ended) return false;
        const uint64_t left = asset->entry->compressed - inflater.consumed;
        if (stream.avail_in == 0 && left > 0) {
            const size_t chunk = left < sizeof inflater.input ? static_cast<size_t>(left) : sizeof inflater.input;
            if (!read_at(asset->manager->fd, inflater.input, chunk, asset->data + inflater.consumed)) return false;
            inflater.consumed += chunk;
            stream.next_in = inflater.input;
            stream.avail_in = static_cast<uInt>(chunk);
        }
        // Without input left, zlib may still finish from what it holds: Z_BUF_ERROR says it cannot.
        const int result = inflate(&stream, Z_NO_FLUSH);
        if (result == Z_STREAM_END) {
            inflater.ended = true;
        } else if (result != Z_OK) {
            return false;
        }
    }
    return true;
}

// The next `size` bytes of the asset, at most 2 GiB. Where its size ends, the stream must end too: data that
// inflates to less or to more than its size is broken.
bool produce(AAsset *asset, unsigned char *out, size_t size) {
    Inflater &inflater = *asset->inflater;
    if (!pull(asset, out, size)) return false;
    inflater.produced += size;
    if (inflater.produced < asset->length || inflater.ended) return true;
    unsigned char extra;
    return !pull(asset, &extra, 1) && inflater.ended && inflater.stream.avail_out == 1;
}

// Reads a deflated asset at its position, inflating from the beginning again for a position before the
// stream's and passing over the bytes before a later one.
bool read_deflated(AAsset *asset, unsigned char *out, size_t size) {
    if (asset->broken) return false;
    if ((asset->inflater == nullptr || asset->position < asset->inflater->produced) && !restart(asset)) {
        return broke(asset);
    }
    Inflater &inflater = *asset->inflater;
    while (inflater.produced < asset->position) {
        const uint64_t gap = asset->position - inflater.produced;
        if (!produce(asset, inflater.passed, gap < sizeof inflater.passed ? static_cast<size_t>(gap)
                                                                         : sizeof inflater.passed)) {
            return broke(asset);
        }
    }
    return produce(asset, out, size) || broke(asset);
}

// A stored asset's bytes where they lie in the APK: its pages, mapped read-only.
const void *map(AAsset *asset) {
    static const unsigned char empty = 0;
    if (asset->length == 0) return &empty;
    if (asset->mapping == nullptr) {
        const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
        const uint64_t start = asset->data / page * page;
        const size_t size = static_cast<size_t>(asset->data - start + asset->length);
        void *mapping = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, asset->manager->fd, static_cast<off_t>(start));
        if (mapping == MAP_FAILED) {
            warn(asset->manager, asset->entry, "cannot be mapped");
            return nullptr;
        }
        asset->mapping = mapping;
        asset->mapping_size = size;
        asset->mapped = static_cast<const unsigned char *>(mapping) + (asset->data - start);
    }
    return asset->mapped;
}

off64_t seek(AAsset *asset, off64_t offset, int whence) {
    if (asset == nullptr) return -1;
    off64_t base;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = static_cast<off64_t>(asset->position); break;
        case SEEK_END: base = static_cast<off64_t>(asset->length); break;
        default: return -1;
    }
    off64_t target;
    if (__builtin_add_overflow(base, offset, &target) || target < 0 ||
        static_cast<uint64_t>(target) > asset->length) {
        return -1;
    }
    asset->position = static_cast<uint64_t>(target);
    return target;
}

// A new descriptor of the APK for a stored asset, with the asset's offset and length in it.
template <typename Offset>
int open_descriptor(AAsset *asset, Offset *start, Offset *length) {
    if (asset == nullptr || asset->entry->method != 0) return -1;  // only a stored asset lies in the APK as it is
    const int fd = open(asset->manager->apk, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (start != nullptr) *start = static_cast<Offset>(asset->data);
    if (length != nullptr) *length = static_cast<Offset>(asset->length);
    return fd;
}

// One APK's manager, without its configuration.
AAssetManager *open_apk(const char *apk) {
    auto *manager = static_cast<AAssetManager *>(calloc(1, sizeof(AAssetManager)));
    if (manager == nullptr) return nullptr;
    manager->fd = -1;
    manager->apk = strdup(apk);
    if (manager->apk == nullptr) {
        destroy(manager);
        return nullptr;
    }
    manager->fd = open(apk, O_RDONLY | O_CLOEXEC);
    if (manager->fd < 0) {
        __android_log_print(ANDROID_LOG_WARN, "libandroid", "%s: %s", apk, strerror(errno));
        destroy(manager);
        return nullptr;
    }
    if (!read_directory(manager)) {
        destroy(manager);
        return nullptr;
    }
    return manager;
}

// The manager of a set: one per APK, each linked to the one before, and the last one returned, which holds the
// configuration. `lenient` skips an APK after the first that cannot be read (an overlay of a Java AssetManager's list
// is no archive of assets), else one failure fails the set.
AAssetManager *create_set(const char *const *apks, size_t count, const struct apkrt_configuration *configuration,
                          bool lenient) {
    if (apks == nullptr || count == 0 || configuration == nullptr) return nullptr;
    AAssetManager *head = nullptr;
    for (size_t index = 0; index < count; ++index) {
        AAssetManager *manager = apks[index] != nullptr ? open_apk(apks[index]) : nullptr;
        if (manager == nullptr) {
            if (lenient && index > 0) {
                __android_log_print(ANDROID_LOG_WARN, "libandroid", "%s: skipped, it cannot be read",
                                    apks[index] != nullptr ? apks[index] : "(null)");
                continue;
            }
            if (head != nullptr) destroy(head);
            return nullptr;
        }
        manager->earlier = head;
        head = manager;
    }
    head->configuration = AConfiguration_new();
    if (head->configuration == nullptr) {
        destroy(head);
        return nullptr;
    }
    minato_configuration_from_session(head->configuration, configuration);
    return head;
}

}  // namespace

extern "C" {

AAssetManager *minato_AAssetManager_create(const char *const *apks, size_t count,
                                           const struct apkrt_configuration *configuration) {
    return create_set(apks, count, configuration, false);
}

AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int) {
    if (mgr == nullptr || filename == nullptr) return nullptr;
    // The last APK of the set first, as Android reads a set: the later one overrides the earlier.
    const Entry *entry = nullptr;
    for (; mgr != nullptr; mgr = mgr->earlier) {
        entry = find(mgr, filename);
        if (entry != nullptr) break;
    }
    if (entry == nullptr) return nullptr;
    const uint64_t data = data_of(mgr, entry);
    if (data == 0) return nullptr;
    auto *asset = static_cast<AAsset *>(calloc(1, sizeof(AAsset)));
    if (asset == nullptr) return nullptr;
    asset->manager = mgr;
    asset->entry = entry;
    asset->data = data;
    asset->length = entry->uncompressed;
    return asset;
}

AAssetDir *AAssetManager_openDir(AAssetManager *mgr, const char *dirName) {
    if (mgr == nullptr || dirName == nullptr) return nullptr;
    // The folder's own prefix below assets/, with its slash.
    size_t prefix_size = strlen(dirName);
    char *prefix = static_cast<char *>(malloc(prefix_size + 2));
    if (prefix == nullptr) return nullptr;
    memcpy(prefix, dirName, prefix_size);
    if (prefix_size > 0 && prefix[prefix_size - 1] != '/') prefix[prefix_size++] = '/';
    prefix[prefix_size] = 0;
    // Its files: the entries with that prefix and no slash after it, in the entries' order.
    // Of a set, the files of all its APKs; a name in several is listed once.
    size_t count = 0, bytes = 0, apks = 0;
    for (const AAssetManager *apk = mgr; apk != nullptr; apk = apk->earlier, ++apks) {
        const size_t first = lower_bound(apk, prefix, prefix_size);
        for (size_t last = first; last < apk->count; ++last) {
            const Entry &entry = apk->entries[last];
            const char *rest = entry.name + kPrefixSize;
            const size_t rest_size = entry.name_size - kPrefixSize;
            if (rest_size < prefix_size || memcmp(rest, prefix, prefix_size) != 0) break;
            if (memchr(rest + prefix_size, '/', rest_size - prefix_size) != nullptr) continue;
            ++count;
            bytes += rest_size - prefix_size + 1;
        }
    }
    auto *folder = static_cast<AAssetDir *>(malloc(sizeof(AAssetDir) + count * sizeof(char *) + bytes));
    if (folder == nullptr) {
        free(prefix);
        return nullptr;
    }
    folder->names = reinterpret_cast<char **>(folder + 1);
    folder->count = 0;
    folder->next = 0;
    char *text = reinterpret_cast<char *>(folder->names + count);
    for (const AAssetManager *apk = mgr; apk != nullptr; apk = apk->earlier) {
        const size_t first = lower_bound(apk, prefix, prefix_size);
        for (size_t index = first; index < apk->count; ++index) {
            const Entry &entry = apk->entries[index];
            const char *rest = entry.name + kPrefixSize;
            const size_t rest_size = entry.name_size - kPrefixSize;
            if (rest_size < prefix_size || memcmp(rest, prefix, prefix_size) != 0) break;
            const char *name = rest + prefix_size;
            const size_t size = rest_size - prefix_size;
            if (memchr(name, '/', size) != nullptr) continue;
            memcpy(text, name, size);
            text[size] = 0;
            folder->names[folder->count++] = text;
            text += size + 1;
        }
    }
    if (apks > 1) {
        qsort(folder->names, folder->count, sizeof(char *), [](const void *a, const void *b) {
            return strcmp(*static_cast<char *const *>(a), *static_cast<char *const *>(b));
        });
        size_t kept = 0;
        for (size_t index = 0; index < folder->count; ++index) {
            if (kept == 0 || strcmp(folder->names[kept - 1], folder->names[index]) != 0) {
                folder->names[kept++] = folder->names[index];
            }
        }
        folder->count = kept;
    }
    free(prefix);
    return folder;
}

const char *AAssetDir_getNextFileName(AAssetDir *assetDir) {
    if (assetDir == nullptr || assetDir->next >= assetDir->count) return nullptr;
    return assetDir->names[assetDir->next++];
}

void AAssetDir_rewind(AAssetDir *assetDir) {
    if (assetDir != nullptr) assetDir->next = 0;
}

void AAssetDir_close(AAssetDir *assetDir) {
    free(assetDir);
}

int AAsset_read(AAsset *asset, void *buf, size_t count) {
    if (asset == nullptr || (buf == nullptr && count > 0)) return -1;
    const uint64_t left = asset->length - asset->position;
    size_t size = count < left ? count : static_cast<size_t>(left);
    if (size > 0x7fffffff) size = 0x7fffffff;
    if (size == 0) return 0;
    auto *out = static_cast<unsigned char *>(buf);
    if (asset->contents != nullptr) {
        memcpy(out, asset->contents + asset->position, size);
    } else if (asset->entry->method != 0) {
        if (!read_deflated(asset, out, size)) return -1;
    } else if (!read_at(asset->manager->fd, out, size, asset->data + asset->position)) {
        return -1;
    }
    asset->position += size;
    return static_cast<int>(size);
}

off_t AAsset_seek(AAsset *asset, off_t offset, int whence) {
    return seek(asset, offset, whence);
}

off64_t AAsset_seek64(AAsset *asset, off64_t offset, int whence) {
    return seek(asset, offset, whence);
}

void AAsset_close(AAsset *asset) {
    if (asset == nullptr) return;
    if (asset->mapping != nullptr) munmap(asset->mapping, asset->mapping_size);
    if (asset->inflater != nullptr) {
        inflateEnd(&asset->inflater->stream);
        free(asset->inflater);
    }
    free(asset->contents);
    free(asset);
}

// A stored asset's bytes in the APK's pages; a deflated one inflated whole into memory, as on Android.
const void *AAsset_getBuffer(AAsset *asset) {
    if (asset == nullptr) return nullptr;
    if (asset->entry->method == 0) return map(asset);
    if (asset->contents == nullptr && !asset->broken) {
        auto *contents = static_cast<unsigned char *>(malloc(asset->length > 0 ? asset->length : 1));
        if (contents == nullptr) return nullptr;
        if (!restart(asset) || !produce(asset, contents, asset->length)) {
            free(contents);
            broke(asset);
            return nullptr;
        }
        asset->contents = contents;
    }
    return asset->contents;
}

off_t AAsset_getLength(AAsset *asset) {
    return asset != nullptr ? static_cast<off_t>(asset->length) : 0;
}

off64_t AAsset_getLength64(AAsset *asset) {
    return asset != nullptr ? static_cast<off64_t>(asset->length) : 0;
}

off_t AAsset_getRemainingLength(AAsset *asset) {
    return asset != nullptr ? static_cast<off_t>(asset->length - asset->position) : 0;
}

off64_t AAsset_getRemainingLength64(AAsset *asset) {
    return asset != nullptr ? static_cast<off64_t>(asset->length - asset->position) : 0;
}

int AAsset_openFileDescriptor(AAsset *asset, off_t *outStart, off_t *outLength) {
    return open_descriptor(asset, outStart, outLength);
}

int AAsset_openFileDescriptor64(AAsset *asset, off64_t *outStart, off64_t *outLength) {
    return open_descriptor(asset, outStart, outLength);
}

// Whether AAsset_getBuffer's bytes lie in memory of their own rather than in the APK's pages.
int AAsset_isAllocated(AAsset *asset) {
    return asset != nullptr && asset->contents != nullptr ? 1 : 0;
}

void AConfiguration_fromAssetManager(AConfiguration *out, AAssetManager *am) {
    if (out != nullptr && am != nullptr) AConfiguration_copy(out, am->configuration);
}

}  // extern "C"

// AAssetManager_fromJava (ADR-0041, decision 7): Android 15 gives the Java AssetManager's native object, the
// AssetManager2 of all its APKs, the same pointer for the object's life (asset_manager.cpp:61-64,
// android-15.0.0_r1). Here it is this file's manager on the app's base APK, one per Java object, kept for the
// process; it reads the base and the splits.
namespace {

// A manager AAssetManager_fromJava made, by the Java object's mObject, with the APK paths it had. A later object at
// the same native address gets a manager of its own when its paths differ, and shares this one, which reads the same
// APKs and is never freed, when they do not.
struct JavaManager {
    jlong object;
    char *paths;  // getApkPaths(), each ended by '\n'
    AAssetManager *manager;
    JavaManager *next;
};

pthread_mutex_t java_managers_lock = PTHREAD_MUTEX_INITIALIZER;
JavaManager *java_managers;

// A pending exception means the framework lacks what AssetManager.java has at the tag: logged and cleared, as
// Android's AAssetManager_fromJava throws nothing.
bool failed(JNIEnv *env, const char *what) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_WARN, "libandroid", "AAssetManager_fromJava: %s failed", what);
    return true;
}

// The paths of getApkPaths() (AssetManager.java:413-425), each ended by '\n', and how many there are; nullptr when
// the call fails.
char *apk_paths(JNIEnv *env, jobject assets, jmethodID get_apk_paths, jsize *count) {
    auto paths = static_cast<jobjectArray>(env->CallObjectMethod(assets, get_apk_paths));
    if (failed(env, "getApkPaths") || paths == nullptr) return nullptr;
    *count = env->GetArrayLength(paths);
    size_t size = 1;
    char *joined = static_cast<char *>(calloc(1, size));
    for (jsize i = 0; joined != nullptr && i < *count; ++i) {
        auto path = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
        const char *chars = path != nullptr ? env->GetStringUTFChars(path, nullptr) : nullptr;
        const size_t length = chars != nullptr ? strlen(chars) : 0;
        char *grown = static_cast<char *>(realloc(joined, size + length + 1));
        if (grown == nullptr) {
            free(joined);
            joined = nullptr;
        } else {
            joined = grown;
            memcpy(joined + size - 1, chars != nullptr ? chars : "", length);
            joined[size - 1 + length] = '\n';
            joined[size + length] = '\0';
            size += length + 1;
        }
        if (chars != nullptr) env->ReleaseStringUTFChars(path, chars);
        env->DeleteLocalRef(path);
    }
    env->DeleteLocalRef(paths);
    return joined;
}

// The `index`th line of `paths`, copied; nullptr past the last.
char *path_at(const char *paths, jsize index) {
    for (jsize i = 0; i < index; ++i) {
        paths = strchr(paths, '\n');
        if (paths == nullptr || *++paths == '\0') return nullptr;
    }
    const char *end = strchr(paths, '\n');
    return end != nullptr && end != paths ? strndup(paths, static_cast<size_t>(end - paths)) : nullptr;
}

// The paths of `assets`, and in `system_count` how many of them are the system's, each lookup checked at once, since
// a JNI call with an exception pending aborts under CheckJNI; nullptr when one fails.
char *paths_of(JNIEnv *env, jobject assets, jclass type, jsize *system_count) {
    const jmethodID get_apk_paths = env->GetMethodID(type, "getApkPaths", "()[Ljava/lang/String;");
    if (failed(env, "AssetManager.getApkPaths")) return nullptr;
    const jmethodID get_system = env->GetStaticMethodID(type, "getSystem", "()Landroid/content/res/AssetManager;");
    if (failed(env, "AssetManager.getSystem")) return nullptr;
    jobject system = env->CallStaticObjectMethod(type, get_system);
    if (failed(env, "AssetManager.getSystem()") || system == nullptr) return nullptr;
    char *const system_paths = apk_paths(env, system, get_apk_paths, system_count);
    env->DeleteLocalRef(system);
    if (system_paths == nullptr) return nullptr;
    free(system_paths);
    jsize count = 0;
    return apk_paths(env, assets, get_apk_paths, &count);
}

}  // namespace

extern "C" AAssetManager *AAssetManager_fromJava(JNIEnv *env, jobject assetManager) {
    if (env == nullptr || assetManager == nullptr) return nullptr;
    jclass type = env->GetObjectClass(assetManager);
    const jfieldID object_field = env->GetFieldID(type, "mObject", "J");
    // A closed AssetManager has no native object any more: Android hands out NULL then.
    const jlong object = failed(env, "AssetManager.mObject") ? 0 : env->GetLongField(assetManager, object_field);
    jsize system_count = 0;
    char *const paths = object != 0 ? paths_of(env, assetManager, type, &system_count) : nullptr;
    env->DeleteLocalRef(type);
    if (paths == nullptr) return nullptr;

    pthread_mutex_lock(&java_managers_lock);
    for (JavaManager *known = java_managers; known != nullptr; known = known->next) {
        if (known->object == object && strcmp(known->paths, paths) == 0) {
            pthread_mutex_unlock(&java_managers_lock);
            free(paths);
            return known->manager;
        }
    }
    // The app's base is the first APK after the system's (AssetManager.java:160-190), then its splits and overlays in
    // the list's order (ADR-0048 decision 9); an overlay holds no assets, and one that cannot be read is skipped.
    char *app_paths[64] = {}, *next_path = nullptr;
    size_t app_count = 0;
    while (app_count < sizeof app_paths / sizeof app_paths[0]) {
        char *next = path_at(paths, system_count + static_cast<jsize>(app_count));
        if (next == nullptr) break;
        app_paths[app_count++] = next;
    }
    if (app_count == sizeof app_paths / sizeof app_paths[0] &&
        (next_path = path_at(paths, system_count + static_cast<jsize>(app_count))) != nullptr) {
        __android_log_print(ANDROID_LOG_WARN, "libandroid", "AAssetManager_fromJava: more than %zu APKs, the rest "
                            "are not read", app_count);
        free(next_path);
    }
    AAssetManager *manager = nullptr;
    struct apkrt_configuration session = {};
    // ENOENT: the host began no session (a check on the Mac), so the session's values stay unset, as ActivityThread
    // keeps its defaults then.
    const int error = syscall(APKRT_NR_SESSION_CONFIGURATION, &session) == 0 ? 0 : errno;
    if (error == ENOENT) session = {};
    if (app_count == 0) {
        __android_log_print(ANDROID_LOG_WARN, "libandroid", "AAssetManager_fromJava: no APK of the app's own");
    } else if (error != 0 && error != ENOENT) {
        __android_log_print(ANDROID_LOG_WARN, "libandroid", "AAssetManager_fromJava: the session: %s", strerror(error));
    } else {
        manager = create_set(app_paths, app_count, &session, true);
    }
    auto *known = manager != nullptr ? static_cast<JavaManager *>(calloc(1, sizeof(JavaManager))) : nullptr;
    if (known != nullptr) {
        *known = JavaManager{object, paths, manager, java_managers};
        java_managers = known;
    } else {
        free(paths);
    }
    pthread_mutex_unlock(&java_managers_lock);
    for (size_t index = 0; index < app_count; ++index) free(app_paths[index]);
    return manager;
}
