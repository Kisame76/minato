// ADR-0041 decision 7's test (asset-ndk-test): what AAssetManager_fromJava and libandroid's AAsset functions answer
// for the Context's AssetManager, one line each, which AssetNdkApplication logs. No line names an address or anything
// else of one device, so that Android 15's log and Minato's compare. stored.ogg is stored in the APK and
// deflated.txt deflated (make-apk's choice by extension); each has 64 lines that name their number.
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/configuration.h>
#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

enum { kMaxLines = 48, kLineSize = 256 };

static char lines[kMaxLines][kLineSize];
static int count;

static void add(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void add(const char *format, ...) {
    if (count == kMaxLines) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(lines[count++], kLineSize, format, arguments);
    va_end(arguments);
}

// The length of the first line of `text`, without its newline.
static int line_length(const char *text, int size) {
    int length = 0;
    while (length < size && text[length] != '\n') ++length;
    return length;
}

// Reads the line at the asset's position and adds it after `what`, with the position the seek gave.
static void read_line(AAsset *asset, const char *name, const char *what, off_t position) {
    char text[128];
    const int got = AAsset_read(asset, text, sizeof text);
    add("%s: %s gives %ld, then \"%.*s\"", name, what, (long)position, line_length(text, got > 0 ? got : 0), text);
}

static void check_asset(AAssetManager *assets, const char *name, int mode) {
    AAsset *asset = AAssetManager_open(assets, name, mode);
    if (asset == NULL) {
        add("%s: NULL", name);
        return;
    }
    char first[128];
    const off_t length = AAsset_getLength(asset);
    const int got = AAsset_read(asset, first, sizeof first);
    const int first_size = line_length(first, got > 0 ? got : 0);
    add("%s: length %ld, length64 %lld; read %d, the first line \"%.*s\", %ld remaining", name, (long)length,
        (long long)AAsset_getLength64(asset), got, first_size, first, (long)AAsset_getRemainingLength(asset));
    // Every line has the first one's length.
    const off_t line = first_size + 1;
    read_line(asset, name, "a seek to line 10", AAsset_seek(asset, 10 * line, SEEK_SET));
    // After line 10 the read stands sizeof first bytes on; two lines on from its start is line 12.
    read_line(asset, name, "a seek on to line 12", AAsset_seek(asset, 2 * line - (off_t)sizeof first, SEEK_CUR));
    read_line(asset, name, "a seek to the last line", AAsset_seek(asset, -line, SEEK_END));
    add("%s: at the end, read %d; a seek past the end gives %ld", name, AAsset_read(asset, first, 1),
        (long)AAsset_seek(asset, length + 1, SEEK_SET));

    const char *buffer = AAsset_getBuffer(asset);
    add("%s: the buffer %s", name,
        buffer == NULL ? "is NULL" : memcmp(buffer, first, (size_t)first_size) == 0 ? "starts with the first line"
                                                                                    : "differs");
    off64_t start = 0, size = 0;
    const int descriptor = AAsset_openFileDescriptor64(asset, &start, &size);
    if (descriptor < 0) {
        add("%s: no descriptor", name);
    } else {
        char through[128];
        const ssize_t read = pread64(descriptor, through, sizeof through, start);
        close(descriptor);
        // Where the data starts depends on the dex file's and the library's sizes, so only what it holds is logged.
        add("%s: a descriptor of %lld bytes, which %s", name, (long long)size,
            read > first_size && memcmp(through, first, (size_t)first_size) == 0 ? "starts with the first line"
                                                                                : "differs");
    }
    AAsset_close(asset);
}

static void list_folder(AAssetManager *assets, const char *name) {
    AAssetDir *folder = AAssetManager_openDir(assets, name);
    if (folder == NULL) {
        add("folder \"%s\": NULL", name);
        return;
    }
    char files[kLineSize] = "";
    int used = 0, found = 0;
    for (const char *file; (file = AAssetDir_getNextFileName(folder)) != NULL; ++found) {
        if (used >= 0 && used < (int)sizeof files) used += snprintf(files + used, sizeof files - (size_t)used, " %s", file);
    }
    AAssetDir_rewind(folder);
    const char *again = AAssetDir_getNextFileName(folder);
    add("folder \"%s\": %d files:%s; after a rewind %s", name, found, files, again != NULL ? again : "nothing");
    AAssetDir_close(folder);
}

JNIEXPORT jobjectArray JNICALL Java_io_github_kisame76_minato_test_assetndk_AssetNdkApplication_check(
    JNIEnv *env, jclass clazz, jobject assets, jobject again) {
    count = 0;
    AAssetManager *manager = AAssetManager_fromJava(env, assets);
    AAssetManager *second = AAssetManager_fromJava(env, assets);
    AAssetManager *other = AAssetManager_fromJava(env, again);
    add("AAssetManager_fromJava: %s; again: %s; through getResources(): %s", manager != NULL ? "a manager" : "NULL",
        second == manager ? "the same" : "another", other == manager ? "the same" : "another");
    if (manager != NULL) {
        check_asset(manager, "stored.ogg", AASSET_MODE_RANDOM);
        check_asset(manager, "deflated.txt", AASSET_MODE_STREAMING);
        add("missing.txt: %s", AAssetManager_open(manager, "missing.txt", AASSET_MODE_UNKNOWN) == NULL ? "NULL" : "found");
        list_folder(manager, "");
        list_folder(manager, "folder");
        list_folder(manager, "missing");
        AConfiguration *configuration = AConfiguration_new();
        AConfiguration_fromAssetManager(configuration, manager);
        add("its configuration: sdkVersion %d", AConfiguration_getSdkVersion(configuration));
        AConfiguration_delete(configuration);
    }

    jclass string = (*env)->FindClass(env, "java/lang/String");
    jobjectArray result = (*env)->NewObjectArray(env, count, string, NULL);
    for (int i = 0; i < count && result != NULL; ++i) {
        jstring text = (*env)->NewStringUTF(env, lines[i]);
        (*env)->SetObjectArrayElement(env, result, i, text);
        (*env)->DeleteLocalRef(env, text);
    }
    return result;
}
