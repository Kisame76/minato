// ADR-0041 a5's test (jnigraphics-test): what the NDK's libjnigraphics answers for two Java bitmaps and the APK's
// assets, one line each, which JnigraphicsApplication logs. AndroidBitmap_getInfo, _lockPixels and _unlockPixels are
// linked (API 8); the functions of API 30 and 31 come through dlsym, as an app of minSdk 29 takes them. No line names
// an address or anything else of one device, so that Android 15's log and Minato's compare. picture.png holds 6 x 4
// RGBA pixels in rows of full, half, quarter and mixed alpha, without color chunks; note.txt is no image.
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/data_space.h>
#include <android/imagedecoder.h>
#include <android/rect.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

enum { kMaxLines = 96, kLineSize = 256 };

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

// The functions of API 30 and 31, by their signatures in the NDK's headers.
struct AHardwareBuffer;
typedef bool (*write_function)(void *context, const void *data, size_t size);
static int32_t (*get_data_space)(JNIEnv *, jobject);
static int (*get_hardware_buffer)(JNIEnv *, jobject, struct AHardwareBuffer **);
static int (*compress)(const AndroidBitmapInfo *, int32_t, const void *, int32_t, int32_t, void *, write_function);
static int (*create_from_asset)(AAsset *, AImageDecoder **);
static int (*create_from_buffer)(const void *, size_t, AImageDecoder **);
static int (*create_from_fd)(int, AImageDecoder **);
static void (*delete_decoder)(AImageDecoder *);
static const AImageDecoderHeaderInfo *(*get_header_info)(const AImageDecoder *);
static int32_t (*header_width)(const AImageDecoderHeaderInfo *);
static int32_t (*header_height)(const AImageDecoderHeaderInfo *);
static const char *(*header_mime_type)(const AImageDecoderHeaderInfo *);
static int32_t (*header_format)(const AImageDecoderHeaderInfo *);
static int (*header_alpha_flags)(const AImageDecoderHeaderInfo *);
static int32_t (*header_data_space)(const AImageDecoderHeaderInfo *);
static size_t (*get_minimum_stride)(AImageDecoder *);
static int (*decode_image)(AImageDecoder *, void *, size_t, size_t);
static int (*set_format)(AImageDecoder *, int32_t);
static int (*set_unpremultiplied)(AImageDecoder *, bool);
static int (*set_target_size)(AImageDecoder *, int32_t, int32_t);
static int (*compute_sampled_size)(const AImageDecoder *, int, int32_t *, int32_t *);
static int (*set_crop)(AImageDecoder *, ARect);
static bool (*is_animated)(AImageDecoder *);
static int32_t (*get_repeat_count)(AImageDecoder *);
static const char *(*result_to_string)(int);

// Looks up `name` in libjnigraphics into `*function`; false, with a line, when it is not there.
static bool find(void *library, const char *name, void **function) {
    *function = library != NULL ? dlsym(library, name) : NULL;
    if (*function == NULL) add("%s: not found", name);
    return *function != NULL;
}

static bool find_all(void) {
    void *library = dlopen("libjnigraphics.so", RTLD_NOW | RTLD_NOLOAD);
    if (library == NULL) library = dlopen("libjnigraphics.so", RTLD_NOW);
    bool found = true;
#define FIND(variable, name) found &= find(library, name, (void **)&variable)
    FIND(get_data_space, "AndroidBitmap_getDataSpace");
    FIND(get_hardware_buffer, "AndroidBitmap_getHardwareBuffer");
    FIND(compress, "AndroidBitmap_compress");
    FIND(create_from_asset, "AImageDecoder_createFromAAsset");
    FIND(create_from_buffer, "AImageDecoder_createFromBuffer");
    FIND(create_from_fd, "AImageDecoder_createFromFd");
    FIND(delete_decoder, "AImageDecoder_delete");
    FIND(get_header_info, "AImageDecoder_getHeaderInfo");
    FIND(header_width, "AImageDecoderHeaderInfo_getWidth");
    FIND(header_height, "AImageDecoderHeaderInfo_getHeight");
    FIND(header_mime_type, "AImageDecoderHeaderInfo_getMimeType");
    FIND(header_format, "AImageDecoderHeaderInfo_getAndroidBitmapFormat");
    FIND(header_alpha_flags, "AImageDecoderHeaderInfo_getAlphaFlags");
    FIND(header_data_space, "AImageDecoderHeaderInfo_getDataSpace");
    FIND(get_minimum_stride, "AImageDecoder_getMinimumStride");
    FIND(decode_image, "AImageDecoder_decodeImage");
    FIND(set_format, "AImageDecoder_setAndroidBitmapFormat");
    FIND(set_unpremultiplied, "AImageDecoder_setUnpremultipliedRequired");
    FIND(set_target_size, "AImageDecoder_setTargetSize");
    FIND(compute_sampled_size, "AImageDecoder_computeSampledSize");
    FIND(set_crop, "AImageDecoder_setCrop");
    FIND(is_animated, "AImageDecoder_isAnimated");
    FIND(get_repeat_count, "AImageDecoder_getRepeatCount");
    FIND(result_to_string, "AImageDecoder_resultToString");
#undef FIND
    return found;
}

// A decoder's result with its name, as AImageDecoder_resultToString gives it.
static const char *named(int result) {
    const char *name = result_to_string(result);
    return name != NULL ? name : "NULL";
}

// `height` rows of `width` pixels from `stride` bytes apart, one line a row after `label`: RGBA_8888 as rrggbbaa,
// RGB_565 as its 16 bits.
static void add_pixels(const char *label, const uint8_t *pixels, int width, int height, size_t stride, int format) {
    for (int y = 0; y < height; ++y) {
        char text[kLineSize];
        int used = 0;
        const uint8_t *row = pixels + (size_t)y * stride;
        for (int x = 0; x < width && used >= 0 && used < (int)sizeof text; ++x) {
            if (format == ANDROID_BITMAP_FORMAT_RGB_565) {
                uint16_t value;
                memcpy(&value, row + 2 * x, sizeof value);
                used += snprintf(text + used, sizeof text - (size_t)used, " %04x", value);
            } else {
                const uint8_t *pixel = row + 4 * x;
                used += snprintf(text + used, sizeof text - (size_t)used, " %02x%02x%02x%02x", pixel[0], pixel[1],
                                 pixel[2], pixel[3]);
            }
        }
        add("%s, row %d:%s", label, y, text);
    }
}

static void check_bitmap(JNIEnv *env, jobject bitmap, const char *name, bool write) {
    AndroidBitmapInfo info;
    memset(&info, 0, sizeof info);
    int result = AndroidBitmap_getInfo(env, bitmap, &info);
    add("%s: getInfo %d: %u x %u, stride %u, format %d, flags 0x%x", name, result, info.width, info.height,
        info.stride, info.format, info.flags);
    add("%s: data space %d", name, get_data_space(env, bitmap));
    struct AHardwareBuffer *buffer = (struct AHardwareBuffer *)1;
    result = get_hardware_buffer(env, bitmap, &buffer);
    add("%s: getHardwareBuffer %d, %s", name, result, buffer == NULL ? "NULL" : "a buffer");
    void *pixels = NULL;
    result = AndroidBitmap_lockPixels(env, bitmap, &pixels);
    add("%s: lockPixels %d, %s", name, result, pixels != NULL ? "pixels" : "NULL");
    if (pixels == NULL) return;
    add_pixels(name, pixels, (int)info.width, (int)info.height, info.stride, info.format);
    if (write) {
        // The first row, premultiplied: opaque, red at half alpha, transparent and a dark blue at quarter alpha.
        static const uint8_t row[16] = {0x11, 0x22, 0x33, 0xff, 0x40, 0x00, 0x00, 0x80,
                                        0x00, 0x00, 0x00, 0x00, 0x08, 0x10, 0x20, 0x40};
        memcpy(pixels, row, sizeof row);
    }
    add("%s: unlockPixels %d", name, AndroidBitmap_unlockPixels(env, bitmap));
}

// AndroidBitmap_compress's output, gathered.
struct output {
    uint8_t *data;
    size_t size, calls;
};

static bool gather(void *context, const void *data, size_t size) {
    struct output *output = context;
    uint8_t *grown = realloc(output->data, output->size + size);
    if (grown == NULL) return false;
    memcpy(grown + output->size, data, size);
    output->data = grown;
    output->size += size;
    ++output->calls;
    return true;
}

// Decodes with `decoder`'s settings at `width` x `height` into a buffer of its minimum stride and adds the pixels.
static void decode(AImageDecoder *decoder, const char *label, int width, int height, int format) {
    const size_t stride = get_minimum_stride(decoder);
    const size_t size = stride * (size_t)height;
    uint8_t *pixels = calloc(1, size);
    if (pixels == NULL) return;
    const int result = decode_image(decoder, pixels, stride, size);
    add("%s: decodeImage %s, stride %zu", label, named(result), stride);
    if (result == ANDROID_IMAGE_DECODER_SUCCESS) add_pixels(label, pixels, width, height, stride, format);
    free(pixels);
}

// The ARGB_8888 bitmap's pixels as PNG, decoded again.
static void check_compress(JNIEnv *env, jobject bitmap) {
    AndroidBitmapInfo info;
    void *pixels = NULL;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS) {
        add("compress: no pixels");
        return;
    }
    struct output output = {NULL, 0, 0};
    const int result = compress(&info, get_data_space(env, bitmap), pixels, ANDROID_BITMAP_COMPRESS_FORMAT_PNG, 100,
                                &output, gather);
    AndroidBitmap_unlockPixels(env, bitmap);
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    add("compress as PNG: %d, %s", result,
        output.size > sizeof signature && memcmp(output.data, signature, sizeof signature) == 0 ? "a PNG" : "no PNG");
    AImageDecoder *decoder = NULL;
    const int created = create_from_buffer(output.data, output.size, &decoder);
    add("the PNG: createFromBuffer %s", named(created));
    if (created == ANDROID_IMAGE_DECODER_SUCCESS) {
        decode(decoder, "the PNG", (int)info.width, (int)info.height, ANDROID_BITMAP_FORMAT_RGBA_8888);
        delete_decoder(decoder);
    }
    free(output.data);
}

static void check_picture(AAssetManager *assets) {
    AAsset *asset = AAssetManager_open(assets, "picture.png", AASSET_MODE_STREAMING);
    AImageDecoder *decoder = NULL;
    const int created = asset != NULL ? create_from_asset(asset, &decoder) : ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    add("picture.png: %s, createFromAAsset %s", asset != NULL ? "opened" : "missing", named(created));
    if (created != ANDROID_IMAGE_DECODER_SUCCESS) {
        if (asset != NULL) AAsset_close(asset);
        return;
    }
    const AImageDecoderHeaderInfo *header = get_header_info(decoder);
    const int width = header_width(header), height = header_height(header);
    add("picture.png: %d x %d, %s, format %d, alpha flags %d, data space %d", width, height, header_mime_type(header),
        header_format(header), header_alpha_flags(header), header_data_space(header));
    add("picture.png: animated %d, repeat count %d", is_animated(decoder), get_repeat_count(decoder));
    decode(decoder, "picture.png", width, height, ANDROID_BITMAP_FORMAT_RGBA_8888);

    add("picture.png: setUnpremultipliedRequired %s", named(set_unpremultiplied(decoder, true)));
    decode(decoder, "picture.png unpremultiplied", width, height, ANDROID_BITMAP_FORMAT_RGBA_8888);
    set_unpremultiplied(decoder, false);

    // One call after the other: C leaves the order of a call's arguments open.
    const int rgb_565 = set_format(decoder, ANDROID_BITMAP_FORMAT_RGB_565);
    const int a_8 = set_format(decoder, ANDROID_BITMAP_FORMAT_A_8);
    const int rgba_f16 = set_format(decoder, ANDROID_BITMAP_FORMAT_RGBA_F16);
    const int rgba_8888 = set_format(decoder, ANDROID_BITMAP_FORMAT_RGBA_8888);
    add("picture.png: setAndroidBitmapFormat RGB_565 %s, A_8 %s, RGBA_F16 %s, RGBA_8888 %s", named(rgb_565), named(a_8),
        named(rgba_f16), named(rgba_8888));

    int32_t sampled_width = 0, sampled_height = 0;
    const int sampled = compute_sampled_size(decoder, 2, &sampled_width, &sampled_height);
    add("picture.png: computeSampledSize 2 %s: %d x %d", named(sampled), sampled_width, sampled_height);
    add("picture.png: setTargetSize 3 x 2 %s", named(set_target_size(decoder, 3, 2)));
    decode(decoder, "picture.png at 3 x 2", 3, 2, ANDROID_BITMAP_FORMAT_RGBA_8888);
    const int zero = set_target_size(decoder, 0, 0);
    const int back = set_target_size(decoder, width, height);
    add("picture.png: setTargetSize 0 x 0 %s, back to 6 x 4 %s", named(zero), named(back));

    const ARect crop = {1, 1, 5, 3};
    add("picture.png: setCrop (1, 1, 5, 3) %s", named(set_crop(decoder, crop)));
    decode(decoder, "picture.png cropped", 4, 2, ANDROID_BITMAP_FORMAT_RGBA_8888);
    const ARect outside = {4, 2, 8, 6}, none = {0, 0, 0, 0};
    const int outside_result = set_crop(decoder, outside);
    const int none_result = set_crop(decoder, none);
    add("picture.png: setCrop (4, 2, 8, 6) %s, none %s", named(outside_result), named(none_result));
    delete_decoder(decoder);
    AAsset_close(asset);

    // The same file through a descriptor of the APK, at the asset's start, when make-apk stored it.
    asset = AAssetManager_open(assets, "picture.png", AASSET_MODE_RANDOM);
    off64_t start = 0, length = 0;
    const int descriptor = asset != NULL ? AAsset_openFileDescriptor64(asset, &start, &length) : -1;
    if (descriptor < 0) {
        add("picture.png: no descriptor");
    } else {
        decoder = NULL;
        const int from_fd = lseek64(descriptor, start, SEEK_SET) == start
                                ? create_from_fd(descriptor, &decoder) : ANDROID_IMAGE_DECODER_SEEK_ERROR;
        add("picture.png: createFromFd %s", named(from_fd));
        if (from_fd == ANDROID_IMAGE_DECODER_SUCCESS) {
            header = get_header_info(decoder);
            add("picture.png through its descriptor: %d x %d, %s", header_width(header), header_height(header),
                header_mime_type(header));
            delete_decoder(decoder);
        }
        close(descriptor);
    }
    if (asset != NULL) AAsset_close(asset);
}

static void check_results(AAssetManager *assets) {
    AAsset *asset = AAssetManager_open(assets, "note.txt", AASSET_MODE_STREAMING);
    AImageDecoder *decoder = NULL;
    const int created = asset != NULL ? create_from_asset(asset, &decoder) : ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    add("note.txt: createFromAAsset %s, %s", named(created), decoder == NULL ? "no decoder" : "a decoder");
    if (decoder != NULL) delete_decoder(decoder);
    if (asset != NULL) AAsset_close(asset);
    add("createFromBuffer of 0 bytes: %s", named(create_from_buffer("", 0, &decoder)));
    for (int result = ANDROID_IMAGE_DECODER_SUCCESS; result >= ANDROID_IMAGE_DECODER_INVALID_STATE; result -= 4) {
        char text[kLineSize];
        int used = 0;
        for (int code = result; code > result - 4 && code >= ANDROID_IMAGE_DECODER_INVALID_STATE; --code) {
            if (used >= 0 && used < (int)sizeof text) {
                used += snprintf(text + used, sizeof text - (size_t)used, " %d %s", code, named(code));
            }
        }
        add("resultToString:%s", text);
    }
    add("resultToString of 1: %s", named(1));
}

JNIEXPORT jobjectArray JNICALL Java_io_github_kisame76_minato_test_jnigraphics_JnigraphicsApplication_check(
    JNIEnv *env, jclass clazz, jobject argb, jobject rgb, jobject java_assets) {
    count = 0;
    if (find_all()) {
        check_bitmap(env, argb, "argb", true);
        check_bitmap(env, rgb, "rgb565", false);
        AndroidBitmapInfo info;
        void *pixels = NULL;
        const int info_result = AndroidBitmap_getInfo(env, NULL, &info);
        const int lock_result = AndroidBitmap_lockPixels(env, NULL, &pixels);
        add("no bitmap: getInfo %d, lockPixels %d, unlockPixels %d", info_result, lock_result,
            AndroidBitmap_unlockPixels(env, NULL));
        check_compress(env, argb);
        AAssetManager *assets = AAssetManager_fromJava(env, java_assets);
        if (assets == NULL) {
            add("AAssetManager_fromJava: NULL");
        } else {
            check_picture(assets);
            check_results(assets);
        }
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
