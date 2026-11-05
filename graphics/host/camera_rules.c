/* The iPhone cameras' rules (ADR-0073 decision 10), as camera_rules.h has them. */
#include "camera_rules.h"

#include <math.h>
#include <string.h>

/* The frame duration the facts give every stream, MINATO_CAMERA_MAX_FPS's, and a JPEG's stall. */
static const int64_t frame_ns = 1000000000 / MINATO_CAMERA_MAX_FPS;
static const int64_t still_stall_ns = 200000000;

static int64_t area(int32_t width, int32_t height) {
    return (int64_t)width * height;
}

static int same_aspect(int32_t w0, int32_t h0, int32_t w1, int32_t h1) {
    return (int64_t)w0 * h1 == (int64_t)w1 * h0;
}

/* A format a stream runs on at all: 8-bit YUV at `fps`. */
static int usable(const struct minato_camera_format *format, int32_t fps) {
    return format->yuv && format->fps_max + 0.5f >= (float)fps;
}

static int takes_still(const struct minato_camera_format *format, int32_t width, int32_t height) {
    for (uint32_t n = 0; n < format->photo_count && n < MINATO_CAMERA_MAX_PHOTO_SIZES; n++) {
        if (format->photo[n][0] == width && format->photo[n][1] == height) return 1;
    }
    return 0;
}

static uint32_t formats_of(const struct minato_camera_device *device) {
    return device->format_count < MINATO_CAMERA_MAX_FORMATS ? device->format_count : MINATO_CAMERA_MAX_FORMATS;
}

/* The largest still within the limit, of `aspect_w`:`aspect_h` when that is not 0. */
static int largest_still(const struct minato_camera_device *device, int32_t aspect_w, int32_t aspect_h,
                         int32_t *width, int32_t *height) {
    int64_t best = 0;
    for (uint32_t f = 0; f < formats_of(device); f++) {
        const struct minato_camera_format *format = &device->formats[f];
        if (!usable(format, MINATO_CAMERA_MAX_FPS)) continue;
        for (uint32_t n = 0; n < format->photo_count && n < MINATO_CAMERA_MAX_PHOTO_SIZES; n++) {
            const int32_t w = format->photo[n][0], h = format->photo[n][1];
            const int64_t size = area(w, h);
            if (w <= 0 || h <= 0 || size > MINATO_CAMERA_MAX_STILL_PIXELS || size <= best) continue;
            if (aspect_w != 0 && !same_aspect(w, h, aspect_w, aspect_h)) continue;
            best = size, *width = w, *height = h;
        }
    }
    return best > 0 ? 0 : -1;
}

int minato_camera_still(const struct minato_camera_device *device, int32_t *width, int32_t *height) {
    return largest_still(device, 0, 0, width, height);
}

int minato_camera_choose_format(const struct minato_camera_device *device, const struct apkrt_camera_stream *streams,
                                uint32_t count, int32_t fps) {
    if (streams == NULL || count == 0) return -1;
    if (fps <= 0) fps = MINATO_CAMERA_MAX_FPS;
    int32_t video_w = 0, video_h = 0, still_w = 0, still_h = 0;
    for (uint32_t n = 0; n < count; n++) {
        const int32_t w = (int32_t)streams[n].width, h = (int32_t)streams[n].height;
        if (streams[n].format == APKRT_CAMERA_FORMAT_JPEG) {
            if (area(w, h) > area(still_w, still_h)) still_w = w, still_h = h;
        } else if (area(w, h) > area(video_w, video_h)) {
            video_w = w, video_h = h;
        }
    }
    int best = -1, best_rank = 3;
    int64_t best_area = 0;
    for (uint32_t f = 0; f < formats_of(device); f++) {
        const struct minato_camera_format *format = &device->formats[f];
        if (!usable(format, fps)) continue;
        if (still_w != 0 && !takes_still(format, still_w, still_h)) continue;
        int rank = 2;  // a still alone: any format with it
        if (video_w != 0) {
            if (format->width < video_w || format->height < video_h) continue;
            if (format->width == video_w && format->height == video_h) {
                rank = 0;
            } else if (same_aspect(format->width, format->height, video_w, video_h)) {
                rank = 1;
            }
        }
        const int64_t size = area(format->width, format->height);
        if (rank < best_rank || (rank == best_rank && size < best_area)) {
            best = (int)f, best_rank = rank, best_area = size;
        }
    }
    return best;
}

/* The facts' list as guest_interface.h has it, written as far as `capacity` reaches. */
struct list {
    uint8_t *out;
    size_t capacity, size;
};

static void add(struct list *list, uint32_t key, uint32_t type, const void *values, uint32_t count) {
    const uint32_t header[3] = {key, type, count};
    const size_t bytes = (size_t)count * (type == APKRT_CAMERA_TYPE_I64 ? 8u : 4u);
    const size_t length = (sizeof header + bytes + 7u) & ~(size_t)7u;
    if (list->size + length <= list->capacity) {
        memcpy(list->out + list->size, header, sizeof header);
        memcpy(list->out + list->size + sizeof header, values, bytes);
        memset(list->out + list->size + sizeof header + bytes, 0, length - sizeof header - bytes);
    }
    list->size += length;
}

static int32_t clamp_i32(int32_t value, int32_t low, int32_t high) {
    return value < low ? low : value > high ? high : value;
}

/* The video sizes the facts list: each usable format's within the limit, once, largest first. */
static uint32_t video_sizes(const struct minato_camera_device *device, int32_t sizes[][2], uint32_t most) {
    uint32_t count = 0;
    for (uint32_t f = 0; f < formats_of(device); f++) {
        const struct minato_camera_format *format = &device->formats[f];
        const int64_t size = area(format->width, format->height);
        if (!usable(format, MINATO_CAMERA_MAX_FPS) || size > MINATO_CAMERA_MAX_VIDEO_PIXELS || format->width <= 0
            || format->height <= 0) {
            continue;
        }
        int known = 0;
        for (uint32_t n = 0; n < count; n++) known |= sizes[n][0] == format->width && sizes[n][1] == format->height;
        if (known || count == most) continue;
        uint32_t at = count++;
        while (at > 0 && area(sizes[at - 1][0], sizes[at - 1][1]) < size) {
            sizes[at][0] = sizes[at - 1][0], sizes[at][1] = sizes[at - 1][1];
            at--;
        }
        sizes[at][0] = format->width, sizes[at][1] = format->height;
    }
    return count;
}

size_t minato_camera_facts(const struct minato_camera_device *device, uint8_t *out, size_t capacity) {
    int32_t still_w = 0, still_h = 0;
    if (minato_camera_still(device, &still_w, &still_h) != 0) return 0;
    const struct apkrt_camera_stream still = {APKRT_CAMERA_FORMAT_JPEG, (uint32_t)still_w, (uint32_t)still_h, 0};
    const int reference = minato_camera_choose_format(device, &still, 1, 0);
    if (reference < 0) return 0;
    const struct minato_camera_format *format = &device->formats[reference];
    struct list list = {out, out == NULL ? 0 : capacity, 0};

    const int32_t facing = device->facing == 1 ? 1 : 0, orientation = facing ? 90 : 270;
    add(&list, APKRT_CAMERA_KEY_FACING, APKRT_CAMERA_TYPE_I32, &facing, 1);
    add(&list, APKRT_CAMERA_KEY_ORIENTATION, APKRT_CAMERA_TYPE_I32, &orientation, 1);
    const int32_t array[2] = {still_w, still_h}, active[4] = {0, 0, still_w, still_h};
    add(&list, APKRT_CAMERA_KEY_PIXEL_ARRAY, APKRT_CAMERA_TYPE_I32, array, 2);
    add(&list, APKRT_CAMERA_KEY_ACTIVE_ARRAY, APKRT_CAMERA_TYPE_I32, active, 4);
    const float focal = MINATO_CAMERA_FOCAL_LENGTH_MM;
    const float width_mm = 2.0f * focal * tanf(format->field_of_view * 3.14159265f / 360.0f);
    const float physical[2] = {width_mm, width_mm * (float)still_h / (float)still_w};
    add(&list, APKRT_CAMERA_KEY_PHYSICAL_SIZE, APKRT_CAMERA_TYPE_F32, physical, 2);
    add(&list, APKRT_CAMERA_KEY_FOCAL_LENGTHS, APKRT_CAMERA_TYPE_F32, &focal, 1);
    add(&list, APKRT_CAMERA_KEY_APERTURES, APKRT_CAMERA_TYPE_F32, &device->aperture, 1);
    const float min_focus = !device->autofocus ? 0.0f : device->min_focus_mm > 0 ? 1000.0f / device->min_focus_mm
                                                                                 : 10.0f;
    add(&list, APKRT_CAMERA_KEY_MIN_FOCUS_DISTANCE, APKRT_CAMERA_TYPE_F32, &min_focus, 1);
    const int32_t flash = device->flash ? 1 : 0;
    add(&list, APKRT_CAMERA_KEY_FLASH, APKRT_CAMERA_TYPE_I32, &flash, 1);
    const int32_t steps = MINATO_CAMERA_BIAS_STEPS_PER_EV, most = MINATO_CAMERA_BIAS_MAX_STEPS;
    const int32_t compensation[4] = {clamp_i32((int32_t)ceilf(device->bias_min * (float)steps), -most, 0),
                                     clamp_i32((int32_t)floorf(device->bias_max * (float)steps), 0, most), 1, steps};
    add(&list, APKRT_CAMERA_KEY_AE_COMPENSATION, APKRT_CAMERA_TYPE_I32, compensation, 4);
    const float wide = device->wide_factor >= 1.0f ? device->wide_factor : 1.0f;
    float zoom_most = format->zoom_max / wide;
    if (zoom_most > MINATO_CAMERA_MAX_ZOOM_RATIO) zoom_most = MINATO_CAMERA_MAX_ZOOM_RATIO;
    if (zoom_most < 1.0f) zoom_most = 1.0f;
    const float zoom[2] = {1.0f / wide, zoom_most};
    add(&list, APKRT_CAMERA_KEY_ZOOM, APKRT_CAMERA_TYPE_F32, zoom, 2);
    const int32_t fps[4] = {15, MINATO_CAMERA_MAX_FPS, MINATO_CAMERA_MAX_FPS, MINATO_CAMERA_MAX_FPS};
    add(&list, APKRT_CAMERA_KEY_FPS_RANGES, APKRT_CAMERA_TYPE_I32, fps, 4);
    const int32_t iso[2] = {(int32_t)lroundf(format->iso_min), (int32_t)lroundf(format->iso_max)};
    add(&list, APKRT_CAMERA_KEY_ISO, APKRT_CAMERA_TYPE_I32, iso, 2);
    const int64_t exposure[2] = {format->exposure_min_ns, format->exposure_max_ns};
    add(&list, APKRT_CAMERA_KEY_EXPOSURE_NS, APKRT_CAMERA_TYPE_I64, exposure, 2);
    // OFF (focus locked), AUTO (a trigger's one focus), CONTINUOUS_VIDEO and _PICTURE; OFF alone without focus.
    const int32_t af[4] = {0, 1, 3, 4};
    add(&list, APKRT_CAMERA_KEY_AF_MODES, APKRT_CAMERA_TYPE_I32, af, device->autofocus ? 4 : 1);
    const int32_t awb = 1, faces = 0;
    add(&list, APKRT_CAMERA_KEY_AWB_MODES, APKRT_CAMERA_TYPE_I32, &awb, 1);
    add(&list, APKRT_CAMERA_KEY_FACES, APKRT_CAMERA_TYPE_I32, &faces, 1);

    // YUV at the video sizes; a JPEG at the largest still, and at the largest of each other aspect they have.
    int32_t sizes[MINATO_CAMERA_MAX_FORMATS][2];
    const uint32_t video = video_sizes(device, sizes, MINATO_CAMERA_MAX_FORMATS);
    int32_t stills[MINATO_CAMERA_MAX_FORMATS][2] = {{still_w, still_h}};
    uint32_t still_count = 1;
    for (uint32_t n = 0; n < video; n++) {
        int32_t w = 0, h = 0;
        if (largest_still(device, sizes[n][0], sizes[n][1], &w, &h) != 0) continue;
        int known = 0;
        for (uint32_t s = 0; s < still_count; s++) known |= stills[s][0] == w && stills[s][1] == h;
        if (known) continue;
        uint32_t at = still_count++;
        while (at > 0 && area(stills[at - 1][0], stills[at - 1][1]) < area(w, h)) {
            stills[at][0] = stills[at - 1][0], stills[at][1] = stills[at - 1][1];
            at--;
        }
        stills[at][0] = w, stills[at][1] = h;
    }
    int64_t streams[4 * 2 * MINATO_CAMERA_MAX_FORMATS], stalls[4 * MINATO_CAMERA_MAX_FORMATS];
    uint32_t entries = 0;
    for (uint32_t n = 0; n < video; n++, entries++) {
        streams[4 * entries] = APKRT_CAMERA_FORMAT_YUV_420_888;
        streams[4 * entries + 1] = sizes[n][0], streams[4 * entries + 2] = sizes[n][1];
        streams[4 * entries + 3] = frame_ns;
    }
    for (uint32_t s = 0; s < still_count; s++, entries++) {
        streams[4 * entries] = APKRT_CAMERA_FORMAT_JPEG;
        streams[4 * entries + 1] = stills[s][0], streams[4 * entries + 2] = stills[s][1];
        streams[4 * entries + 3] = frame_ns;
        stalls[4 * s] = APKRT_CAMERA_FORMAT_JPEG, stalls[4 * s + 1] = stills[s][0];
        stalls[4 * s + 2] = stills[s][1], stalls[4 * s + 3] = still_stall_ns;
    }
    add(&list, APKRT_CAMERA_KEY_STREAMS, APKRT_CAMERA_TYPE_I64, streams, 4 * entries);
    add(&list, APKRT_CAMERA_KEY_STALLS, APKRT_CAMERA_TYPE_I64, stalls, 4 * still_count);
    return list.size;
}

void minato_camera_3a_trigger(struct minato_camera_3a *state, uint8_t af_trigger, uint8_t ae_precapture) {
    if (af_trigger == 1) state->focus_phase = 1, state->focus_frames = 0;
    if (af_trigger == 2) state->focus_phase = 0;
    if (ae_precapture == 1) state->exposure_phase = 1, state->exposure_frames = 0;
    if (ae_precapture == 2) state->exposure_phase = 0;
}

/* One frame of a trigger's phase: 1 while it goes on, 0 once it is done. */
static int step(uint8_t *phase, uint32_t *frames, uint8_t adjusting) {
    if (*phase == 1 && adjusting) *phase = 2, *frames = 0;
    const uint32_t most = *phase == 1 ? MINATO_CAMERA_TRIGGER_START_FRAMES : MINATO_CAMERA_TRIGGER_MOST_FRAMES;
    if ((*phase == 2 && !adjusting) || *frames >= most) {
        *phase = 0;
        return 0;
    }
    (*frames)++;
    return 1;
}

uint32_t minato_camera_3a_frame(struct minato_camera_3a *state, const struct minato_camera_adjusting *now) {
    uint32_t states = 0;
    if (!now->autofocus) {
        state->focus_phase = 0;
        states |= APKRT_CAMERA_STATE_FOCUSED;
    } else if (state->focus_phase != 0) {
        states |= step(&state->focus_phase, &state->focus_frames, now->focus) ? APKRT_CAMERA_STATE_FOCUSING
                                                                              : APKRT_CAMERA_STATE_FOCUSED;
    } else {
        states |= now->focus ? APKRT_CAMERA_STATE_FOCUSING : APKRT_CAMERA_STATE_FOCUSED;
    }
    if (state->exposure_phase != 0) {
        if (step(&state->exposure_phase, &state->exposure_frames, now->exposure)) states |= APKRT_CAMERA_STATE_EXPOSING;
    } else if (now->exposure) {
        states |= APKRT_CAMERA_STATE_EXPOSING;
    }
    if (now->white_balance) states |= APKRT_CAMERA_STATE_BALANCING;
    if (now->flash_available) states |= APKRT_CAMERA_STATE_FLASH_READY;
    return states;
}

float minato_camera_zoom_factor(const struct minato_camera_device *device, float ratio, float zoom_max) {
    const float wide = device->wide_factor >= 1.0f ? device->wide_factor : 1.0f;
    const float factor = ratio * wide;
    return factor < 1.0f ? 1.0f : factor > zoom_max ? (zoom_max < 1.0f ? 1.0f : zoom_max) : factor;
}

float minato_camera_bias(const struct minato_camera_device *device, int32_t steps) {
    const float bias = (float)steps / (float)MINATO_CAMERA_BIAS_STEPS_PER_EV;
    return bias < device->bias_min ? device->bias_min : bias > device->bias_max ? device->bias_max : bias;
}

int minato_camera_point(int32_t facing, const int32_t region[5], int32_t width, int32_t height, float *x, float *y) {
    if (region[4] <= 0 || width <= 0 || height <= 0) return 0;
    float px = ((float)region[0] + (float)region[2]) / 2.0f / (float)width;
    float py = ((float)region[1] + (float)region[3]) / 2.0f / (float)height;
    px = px < 0 ? 0 : px > 1 ? 1 : px;
    py = py < 0 ? 0 : py > 1 ? 1 : py;
    // The front camera's frames are AVFoundation's picture turned by 180 (minato_camera_frame_rotation).
    *x = facing == 1 ? px : 1.0f - px;
    *y = facing == 1 ? py : 1.0f - py;
    return 1;
}

int32_t minato_camera_frame_rotation(int32_t facing) {
    return facing == 1 ? 0 : 180;
}

int32_t minato_camera_still_rotation(int32_t facing, int32_t jpeg_orientation) {
    const int32_t turn = ((jpeg_orientation % 360) + 360) % 360;
    return facing == 1 ? turn : (turn + 180) % 360;
}
