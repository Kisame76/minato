// asset-test: libandroid's asset manager as activity-host makes it (ADR-0028, decision 10) on archives this
// test writes, sound and broken ones, and AConfiguration_fromAssetManager with the values decision 9
// chooses from the session's configuration. What Android fixes for assets, activity-test compares with
// Android 15; this test adds what only Minato has: minato_AAssetManager_create, APKs no installer would
// accept, and minato_AConfiguration_fromSession without a session. A broken central directory makes the
// manager fail, a broken entry only that entry.
// A test of the runtime: Android has no minato_AAssetManager_create.
// Expected output: fixtures/expected/asset-test.out
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "APKRuntime/guest_interface.h"

static AAssetManager *(*create)(const char *, const struct apkrt_configuration *);

static const struct apkrt_configuration phone = {3.0, 1290, 2796, 1, 1, 120, "de", "DE"};

// MARK: - Writing archives

struct archive {
    unsigned char bytes[1 << 20];
    size_t size;
    unsigned char central[8192];
    size_t central_size;
    unsigned count;
    size_t eocd;  // where the end of central directory record starts, once finished
};

static void put16(unsigned char *at, unsigned value) {
    at[0] = (unsigned char)value;
    at[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *at, uint32_t value) {
    put16(at, value & 0xffff);
    put16(at + 2, value >> 16);
}

// An entry with its local header in `bytes` and its central directory header in `central`; returns where
// its central header starts. Method 0 stores, 8 deflates. The name may hold a NUL byte.
static size_t add_named(struct archive *zip, const char *name, size_t name_size, const char *contents, size_t size,
                        int method) {
    static unsigned char data[1 << 20];
    size_t data_size = size;
    if (method == 8) {
        z_stream stream = {0};
        deflateInit2(&stream, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        stream.next_in = (unsigned char *)contents;
        stream.avail_in = (unsigned)size;
        stream.next_out = data;
        stream.avail_out = sizeof data;
        deflate(&stream, Z_FINISH);
        data_size = stream.total_out;
        deflateEnd(&stream);
    } else {
        memcpy(data, contents, size);
    }
    const uint32_t crc = (uint32_t)crc32(0, (const unsigned char *)contents, (unsigned)size);
    const size_t local = zip->size;
    unsigned char *header = zip->bytes + zip->size;
    memset(header, 0, 30);
    put32(header, 0x04034b50);
    put16(header + 4, 20);
    put16(header + 8, (unsigned)method);
    put32(header + 14, crc);
    put32(header + 18, (uint32_t)data_size);
    put32(header + 22, (uint32_t)size);
    put16(header + 26, (unsigned)name_size);
    memcpy(header + 30, name, name_size);
    memcpy(header + 30 + name_size, data, data_size);
    zip->size += 30 + name_size + data_size;

    const size_t at = zip->central_size;
    unsigned char *entry = zip->central + at;
    memset(entry, 0, 46);
    put32(entry, 0x02014b50);
    put16(entry + 4, 20);
    put16(entry + 6, 20);
    put16(entry + 10, (unsigned)method);
    put32(entry + 16, crc);
    put32(entry + 20, (uint32_t)data_size);
    put32(entry + 24, (uint32_t)size);
    put16(entry + 28, (unsigned)name_size);
    put32(entry + 42, (uint32_t)local);
    memcpy(entry + 46, name, name_size);
    zip->central_size += 46 + name_size;
    ++zip->count;
    return at;
}

static size_t add(struct archive *zip, const char *name, const char *contents, size_t size, int method) {
    return add_named(zip, name, strlen(name), contents, size, method);
}

// Appends the central directory and the end record, with an optional comment.
static void finish(struct archive *zip, const char *comment, size_t comment_size) {
    const size_t central = zip->size;
    memcpy(zip->bytes + zip->size, zip->central, zip->central_size);
    zip->size += zip->central_size;
    unsigned char *end = zip->bytes + zip->size;
    zip->eocd = zip->size;
    memset(end, 0, 22);
    put32(end, 0x06054b50);
    put16(end + 8, zip->count);
    put16(end + 10, zip->count);
    put32(end + 12, (uint32_t)zip->central_size);
    put32(end + 16, (uint32_t)central);
    put16(end + 20, (unsigned)comment_size);
    memcpy(end + 22, comment, comment_size);
    zip->size += 22 + comment_size;
}

static const char deflated[] =
    "A deflated asset, which repeats itself. A deflated asset, which repeats itself. A deflated asset, which "
    "repeats itself. A deflated asset, which repeats itself. A deflated asset, which repeats itself. The end.\n";
static const char stored[] = "Stored bytes, 21 long";
// Bytes that hardly deflate, so that their compressed data spans several of the reader's 64 KiB pieces.
static unsigned char big[204800];

static void fill_big(void) {
    uint32_t state = 1;
    for (size_t index = 0; index < sizeof big; ++index) {
        state = state * 1103515245u + 12345u;
        big[index] = (unsigned char)(state >> 24);
    }
}

// The sound archive every case starts from: a folder's own entry and a name with a NUL byte, which no
// asset name can reach, and a comment that holds a false end record: its signature and, where its
// comment's length would be, "re".
static void sound(struct archive *zip) {
    memset(zip, 0, sizeof *zip);
    add(zip, "AndroidManifest.xml", "manifest", 8, 8);
    add(zip, "assets/d.txt", deflated, sizeof deflated - 1, 8);
    add(zip, "assets/s.bin", stored, sizeof stored - 1, 0);
    add(zip, "assets/dir/b.txt", "b", 1, 0);
    add(zip, "assets/dir/a.txt", "a", 1, 8);
    add(zip, "assets/dir/sub/c.txt", "c", 1, 0);
    add(zip, "assets/dir/", "", 0, 0);
    add_named(zip, "assets/nul\0.txt", 15, "nul", 3, 0);
    add(zip, "assets/big.bin", (const char *)big, sizeof big, 8);
    add(zip, "lib/arm64-v8a/libx.so", "library", 7, 0);
    finish(zip, "PK\5\6 is not the end record", 26);
}

static int write_file(const char *path, const unsigned char *bytes, size_t size) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    const ssize_t written = write(fd, bytes, size);
    close(fd);
    return written == (ssize_t)size ? 0 : -1;
}

static AAssetManager *manager_of(const struct archive *zip) {
    if (write_file("test.apk", zip->bytes, zip->size) != 0) {
        printf("cannot write test.apk\n");
        return NULL;
    }
    return create("/data/local/tmp/test.apk", &phone);
}

// MARK: - Reading assets

static uint32_t hash(const unsigned char *data, size_t size) {
    uint32_t value = 2166136261u;
    for (size_t index = 0; index < size; ++index) value = (value ^ data[index]) * 16777619u;
    return value;
}

static void read_all(AAssetManager *assets, const char *name) {
    AAsset *asset = AAssetManager_open(assets, name, AASSET_MODE_STREAMING);
    if (asset == NULL) {
        printf("%s: NULL\n", name);
        return;
    }
    unsigned char contents[1024];
    size_t total = 0;
    int reads = 0, got;
    while ((got = AAsset_read(asset, contents + total, 64)) > 0) {
        total += (size_t)got;
        ++reads;
    }
    printf("%s: length %ld, %zu bytes in %d reads, hash %08x, then %d\n", name, (long)AAsset_getLength(asset), total,
           reads, hash(contents, total), got);
    AAsset_close(asset);
}

static void check_seeks(AAssetManager *assets) {
    AAsset *asset = AAssetManager_open(assets, "d.txt", AASSET_MODE_RANDOM);
    if (asset == NULL) {
        printf("d.txt: NULL\n");
        return;
    }
    char last = 0;
    printf("seek: set 10 %ld, cur 5 %ld, end -1 %ld", (long)AAsset_seek(asset, 10, SEEK_SET),
           (long)AAsset_seek(asset, 5, SEEK_CUR), (long)AAsset_seek(asset, -1, SEEK_END));
    printf(", read %d '%s'", AAsset_read(asset, &last, 1), last == '\n' ? "\\n" : "?");
    const off_t length = (off_t)sizeof deflated - 1;
    printf(", set -1 %ld, set past the end %ld, cur 0 %ld", (long)AAsset_seek(asset, -1, SEEK_SET),
           (long)AAsset_seek(asset, length + 1, SEEK_SET), (long)AAsset_seek(asset, 0, SEEK_CUR));
    printf(", set to the end %ld, read %d", (long)AAsset_seek64(asset, length, SEEK_SET), AAsset_read(asset, &last, 1));
    printf(", whence 7 %ld, set 50 %ld, remaining %ld\n", (long)AAsset_seek(asset, 0, 7),
           (long)AAsset_seek(asset, 50, SEEK_SET), (long)AAsset_getRemainingLength64(asset));
    AAsset_close(asset);
}

static void check_stored(AAssetManager *assets) {
    AAsset *asset = AAssetManager_open(assets, "s.bin", AASSET_MODE_BUFFER);
    if (asset == NULL) {
        printf("s.bin: NULL\n");
        return;
    }
    off64_t start = 0, length = 0;
    const int fd = AAsset_openFileDescriptor64(asset, &start, &length);
    char bytes[64] = "";
    const ssize_t got = fd >= 0 ? pread64(fd, bytes, (size_t)length, start) : -1;
    if (fd >= 0) close(fd);
    const void *buffer = AAsset_getBuffer(asset);
    printf("s.bin: descriptor %s, length %ld, \"%.*s\"; the buffer %s, allocated %d\n", fd >= 0 ? "given" : "refused",
           (long)length, got > 0 ? (int)got : 0, bytes,
           buffer != NULL && got == (ssize_t)length && memcmp(buffer, bytes, (size_t)got) == 0 ? "the same" : "differs",
           AAsset_isAllocated(asset));
    AAsset_close(asset);
    asset = AAssetManager_open(assets, "d.txt", AASSET_MODE_BUFFER);
    const unsigned char *inflated = asset != NULL ? AAsset_getBuffer(asset) : NULL;
    printf("d.txt: descriptor %s; the buffer %s, allocated %d\n", asset != NULL && AAsset_openFileDescriptor(asset, &(off_t){0},
                                                                                          &(off_t){0}) >= 0
                                                     ? "given" : "refused",
           inflated != NULL && memcmp(inflated, deflated, sizeof deflated - 1) == 0 ? "the text" : "differs",
           asset != NULL ? AAsset_isAllocated(asset) : -1);
    if (asset != NULL) AAsset_close(asset);
}

// A deflated asset of several pieces of compressed data, read in pieces, then back and on again.
static void check_big(AAssetManager *assets) {
    AAsset *asset = AAssetManager_open(assets, "big.bin", AASSET_MODE_STREAMING);
    if (asset == NULL) {
        printf("big.bin: NULL\n");
        return;
    }
    static unsigned char contents[sizeof big];
    size_t total = 0;
    int reads = 0, got;
    while (total < sizeof contents && (got = AAsset_read(asset, contents + total, 4096)) > 0) {
        total += (size_t)got;
        ++reads;
    }
    const int end = AAsset_read(asset, contents, 1);
    printf("big.bin: length %ld, %zu bytes in %d reads, %s, then %d", (long)AAsset_getLength(asset), total, reads,
           total == sizeof big && memcmp(contents, big, sizeof big) == 0 ? "the same bytes" : "other bytes", end);
    unsigned char piece[100];
    printf(", back to 1000: %ld", (long)AAsset_seek(asset, 1000, SEEK_SET));
    got = AAsset_read(asset, piece, sizeof piece);
    printf(" %s", got == 100 && memcmp(piece, big + 1000, 100) == 0 ? "the same" : "other bytes");
    printf(", on to 150000: %ld", (long)AAsset_seek(asset, 150000, SEEK_SET));
    got = AAsset_read(asset, piece, sizeof piece);
    printf(" %s\n", got == 100 && memcmp(piece, big + 150000, 100) == 0 ? "the same" : "other bytes");
    AAsset_close(asset);
}

// A stored asset in pieces of 5 bytes, then from the middle.
static void check_pieces(AAssetManager *assets) {
    AAsset *asset = AAssetManager_open(assets, "s.bin", AASSET_MODE_STREAMING);
    if (asset == NULL) {
        printf("s.bin: NULL\n");
        return;
    }
    char text[64] = "";
    size_t total = 0;
    int got;
    printf("s.bin in pieces of 5:");
    while ((got = AAsset_read(asset, text + total, 5)) > 0) {
        printf(" %d", got);
        total += (size_t)got;
    }
    printf(" %d, \"%.*s\"", got, (int)total, text);
    char piece[8] = "";
    const long at = (long)AAsset_seek(asset, 7, SEEK_SET);
    got = AAsset_read(asset, piece, 5);
    printf("; from %ld: \"%.*s\"\n", at, got > 0 ? got : 0, piece);
    AAsset_close(asset);
}

// What a thread with a small stack reads, as an app's loader or audio thread may have one.
struct small_stack {
    AAssetManager *assets;
    size_t deflated, big;
    int same;
};

static size_t read_whole(AAssetManager *assets, const char *name, unsigned char *contents, size_t size) {
    AAsset *asset = AAssetManager_open(assets, name, AASSET_MODE_STREAMING);
    size_t total = 0;
    int got;
    while (asset != NULL && total < size && (got = AAsset_read(asset, contents + total, 4096)) > 0) total += (size_t)got;
    if (asset != NULL) AAsset_close(asset);
    return total;
}

static void *read_on_small_stack(void *argument) {
    struct small_stack *run = argument;
    static unsigned char contents[sizeof big];
    run->deflated = read_whole(run->assets, "d.txt", contents, sizeof contents);
    run->big = read_whole(run->assets, "big.bin", contents, sizeof contents);
    run->same = run->big == sizeof big && memcmp(contents, big, sizeof big) == 0;
    return NULL;
}

static void check_small_stack(AAssetManager *assets) {
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 32 * 1024);
    struct small_stack run = {assets, 0, 0, 0};
    pthread_t thread;
    if (pthread_create(&thread, &attributes, read_on_small_stack, &run) != 0) {
        printf("no thread\n");
        return;
    }
    pthread_join(thread, NULL);
    printf("a thread with a 32 KiB stack: d.txt %zu bytes, big.bin %zu bytes, %s\n", run.deflated, run.big,
           run.same ? "the same" : "other bytes");
}

static void list(AAssetManager *assets, const char *folder) {
    AAssetDir *directory = AAssetManager_openDir(assets, folder);
    if (directory == NULL) {
        printf("folder \"%s\": NULL\n", folder);
        return;
    }
    printf("folder \"%s\":", folder);
    for (const char *name; (name = AAssetDir_getNextFileName(directory)) != NULL;) printf(" %s", name);
    AAssetDir_rewind(directory);
    const char *first = AAssetDir_getNextFileName(directory);
    printf(" (after a rewind: %s)\n", first != NULL ? first : "nothing");
    AAssetDir_close(directory);
}

static void check_names(AAssetManager *assets) {
    static const char *const names[] = {"dir", "dir/", "", "/d.txt", "dir/../d.txt", "libx.so",
                                        "../lib/arm64-v8a/libx.so", "D.TXT", "dir/sub/c.txt"};
    printf("names:");
    for (size_t index = 0; index < sizeof names / sizeof names[0]; ++index) {
        AAsset *asset = AAssetManager_open(assets, names[index], AASSET_MODE_UNKNOWN);
        printf(" \"%s\" %s", names[index], asset != NULL ? "found" : "NULL");
        if (asset != NULL) AAsset_close(asset);
    }
    printf("\n");
}

static void print_configuration(AAssetManager *assets) {
    AConfiguration *config = AConfiguration_new();
    AConfiguration_fromAssetManager(config, assets);
    char language[2], country[2];
    AConfiguration_getLanguage(config, language);
    AConfiguration_getCountry(config, country);
    printf("configuration: locale %c%c-%c%c, density %d, orientation %d, touchscreen %d, keyboard %d, keys hidden %d, "
           "navigation %d, navigation hidden %d, mcc %d, mnc %d\n",
           language[0] ? language[0] : '-', language[1] ? language[1] : '-', country[0] ? country[0] : '-',
           country[1] ? country[1] : '-', AConfiguration_getDensity(config), AConfiguration_getOrientation(config),
           AConfiguration_getTouchscreen(config), AConfiguration_getKeyboard(config),
           AConfiguration_getKeysHidden(config), AConfiguration_getNavigation(config),
           AConfiguration_getNavHidden(config), AConfiguration_getMcc(config), AConfiguration_getMnc(config));
    printf("  screen size %d, long %d, round %d, %d x %d dp, smallest %d dp, layout direction %d, UI mode %d, night %d, "
           "sdk %d, grammatical gender %d\n",
           AConfiguration_getScreenSize(config), AConfiguration_getScreenLong(config),
           AConfiguration_getScreenRound(config), AConfiguration_getScreenWidthDp(config),
           AConfiguration_getScreenHeightDp(config), AConfiguration_getSmallestScreenWidthDp(config),
           AConfiguration_getLayoutDirection(config), AConfiguration_getUiModeType(config),
           AConfiguration_getUiModeNight(config), AConfiguration_getSdkVersion(config),
           AConfiguration_getGrammaticalGender(config));
    AConfiguration_delete(config);
}

// The screen's values for a session with that scale and size.
static void print_screen(double scale, int32_t width, int32_t height) {
    const struct apkrt_configuration session = {scale, width, height, 1, 1, 60, "en", "US"};
    AConfiguration *config = AConfiguration_new();
    AConfiguration_fromAssetManager(config, create("/data/local/tmp/test.apk", &session));
    printf("screen %d x %d at %g: %d x %d dp, smallest %d, size %d, long %d, density %d\n", width, height, scale,
           AConfiguration_getScreenWidthDp(config), AConfiguration_getScreenHeightDp(config),
           AConfiguration_getSmallestScreenWidthDp(config), AConfiguration_getScreenSize(config),
           AConfiguration_getScreenLong(config), AConfiguration_getDensity(config));
    AConfiguration_delete(config);
}

// MARK: - Broken archives and entries

// An archive whose central directory is broken: the manager must not come to be.
static void broken(const char *what, struct archive *zip) {
    printf("broken, %s: %s\n", what, manager_of(zip) == NULL ? "NULL" : "created");
}

static uint32_t get32(const unsigned char *at) {
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

// The sound archive with the central header of d.txt (the second entry) or its local header changed:
// only that entry fails.
static void broken_entry(const char *what, int central_offset, int local_offset, uint32_t value, int width) {
    static struct archive zip;
    sound(&zip);
    const size_t central = get32(zip.bytes + zip.eocd + 16);
    unsigned char *entry = zip.bytes + central + 46 + strlen("AndroidManifest.xml");
    unsigned char *field = central_offset >= 0 ? entry + central_offset : zip.bytes + get32(entry + 42) + local_offset;
    if (width == 2) put16(field, value); else put32(field, value);
    AAssetManager *assets = manager_of(&zip);
    if (assets == NULL) {
        printf("entry, %s: no manager\n", what);
        return;
    }
    AAsset *asset = AAssetManager_open(assets, "d.txt", AASSET_MODE_UNKNOWN);
    AAsset *other = AAssetManager_open(assets, "s.bin", AASSET_MODE_UNKNOWN);
    printf("entry, %s: %s", what, asset == NULL ? "NULL" : "found");
    if (asset != NULL) {
        // Streamed, a broken asset reads until the damage.
        char piece[64];
        size_t total = 0;
        int got;
        while ((got = AAsset_read(asset, piece, sizeof piece)) > 0) total += (size_t)got;
        printf(", read %zu bytes, then %d, buffer %s", total, got, AAsset_getBuffer(asset) != NULL ? "given" : "NULL");
        AAsset_close(asset);
    }
    printf("; s.bin %s\n", other != NULL ? "found" : "NULL");
    if (other != NULL) AAsset_close(other);
}

int main(void) {
    void *android = dlopen("libandroid.so", RTLD_NOW);
    create = (AAssetManager * (*)(const char *, const struct apkrt_configuration *)) dlsym(android,
                                                                                      "minato_AAssetManager_create");
    if (create == NULL) {
        printf("no minato_AAssetManager_create\n");
        return 1;
    }

    fill_big();
    static struct archive zip;
    sound(&zip);
    AAssetManager *assets = manager_of(&zip);
    printf("sound: %s\n", assets != NULL ? "created" : "NULL");
    if (assets == NULL) return 1;
    read_all(assets, "d.txt");
    read_all(assets, "s.bin");
    read_all(assets, "dir/a.txt");
    check_seeks(assets);
    check_stored(assets);
    check_big(assets);
    check_pieces(assets);
    check_small_stack(assets);
    list(assets, "");
    list(assets, "dir");
    list(assets, "dir/");
    list(assets, "dir/sub");
    list(assets, "missing");
    list(assets, "d.txt");
    check_names(assets);
    print_configuration(assets);
    // A tablet in landscape at scale 2, at night, in Arabic without a country.
    const struct apkrt_configuration tablet = {2.0, 2732, 2048, 2, 2, 120, "ar", ""};
    print_configuration(create("/data/local/tmp/test.apk", &tablet));
    // The borders of Android's screen sizes and of a long screen, and a density that rounds.
    print_screen(2, 1920, 1440);
    print_screen(2, 1918, 1440);
    print_screen(2, 1280, 960);
    print_screen(2, 1278, 960);
    print_screen(2, 940, 640);
    print_screen(2, 938, 640);
    print_screen(2, 1000, 602);
    print_screen(2, 1000, 604);
    print_screen(2.61, 1080, 2340);
    printf("missing file: %s\n", create("/data/local/tmp/missing.apk", &phone) == NULL ? "NULL" : "created");
    // The session's configuration for libandroid_runtime (ADR-0039, config-test): this program runs without a
    // session, so only the errors; rotation-test compares it with the asset manager's in a session.
    int (*from_session)(AConfiguration *, struct apkrt_configuration *) =
        (int (*)(AConfiguration *, struct apkrt_configuration *))dlsym(android, "minato_AConfiguration_fromSession");
    if (from_session == NULL) {
        printf("no minato_AConfiguration_fromSession\n");
    } else {
        AConfiguration *config = AConfiguration_new();
        struct apkrt_configuration session;
        const int without_session = from_session(config, &session);
        const int without_config = from_session(NULL, &session);
        const int without_struct = from_session(config, NULL);
        printf("configuration from the session: without one %d, without a configuration %d, without a struct %d\n",
               without_session, without_config, without_struct);
        AConfiguration_delete(config);
    }

    memset(&zip, 0, sizeof zip);
    broken("empty file", &zip);
    memcpy(zip.bytes, "not an archive at all", 21);
    zip.size = 21;
    broken("no end record", &zip);
    sound(&zip);
    put16(zip.bytes + zip.eocd + 20, 27);
    broken("comment longer than the file", &zip);
    sound(&zip);
    put32(zip.bytes + zip.eocd + 12, get32(zip.bytes + zip.eocd + 12) + 1);
    broken("central directory into the end record", &zip);
    sound(&zip);
    put16(zip.bytes + zip.eocd + 8, zip.count + 1);
    put16(zip.bytes + zip.eocd + 10, zip.count + 1);
    broken("one entry more than there are", &zip);
    sound(&zip);
    put16(zip.bytes + zip.eocd + 8, zip.count - 1);
    broken("entries on this disk and in all differ", &zip);
    sound(&zip);
    put32(zip.bytes + zip.eocd + 16, 0xffffffff);
    broken("ZIP64", &zip);
    sound(&zip);
    put16(zip.bytes + zip.eocd + 4, 1);
    broken("split archive", &zip);
    sound(&zip);
    put32(zip.bytes + get32(zip.bytes + zip.eocd + 16), 0x02014b51);
    broken("central header signature", &zip);
    sound(&zip);
    // The last central header's name runs past the directory.
    put16(zip.bytes + zip.eocd - 46 - strlen("lib/arm64-v8a/libx.so") + 28, 200);
    broken("name past the directory", &zip);
    memset(&zip, 0, sizeof zip);
    add(&zip, "assets/twice.txt", "one", 3, 0);
    add(&zip, "assets/twice.txt", "two", 3, 0);
    finish(&zip, "", 0);
    broken("two entries of one name", &zip);

    broken_entry("local header past the file", 42, 0, 0x7fffffff, 4);
    broken_entry("local header signature", -1, 0, 0x04034b51, 4);
    broken_entry("local name differs", -1, 30, 0x58585858, 4);
    broken_entry("data into the central directory", 20, 0, 0x1000000, 4);
    broken_entry("method 12", 10, 0, 12, 2);
    broken_entry("encrypted", 8, 0, 1, 2);
    broken_entry("ZIP64 size", 24, 0, 0xffffffff, 4);
    broken_entry("inflates to less", 24, 0, sizeof deflated, 4);
    broken_entry("inflates to more", 24, 0, sizeof deflated - 2, 4);
    broken_entry("corrupt data", -1, 30 + (int)strlen("assets/d.txt"), 0xffffffff, 4);
    // A stored entry whose sizes differ.
    sound(&zip);
    {
        const size_t central = get32(zip.bytes + zip.eocd + 16);
        unsigned char *entry = zip.bytes + central;
        for (int index = 0; index < 2; ++index) entry += 46 + (index == 0 ? 19 : 12);  // to s.bin's header
        put32(entry + 24, 22);
        AAssetManager *manager = manager_of(&zip);
        printf("entry, stored sizes differ: %s\n",
               manager == NULL ? "no manager"
               : AAssetManager_open(manager, "s.bin", AASSET_MODE_UNKNOWN) == NULL ? "NULL" : "found");
    }
    return 0;
}
