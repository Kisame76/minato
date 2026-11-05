/* ADR-0073 decision 10: the rules of the iPhone's cameras (camera_avf.m) that need no AVFoundation, so that the Mac's
   tests run them (graphics.camera_rules): a camera's facts from what AVCaptureDevice tells of it, the device format a
   stream configuration runs on, the frames' 3A state bits from the device's isAdjusting* answers, and the mapping of
   a request's controls onto the device (zoom factor, exposure bias, points of interest, rotations). Plain C, no
   allocation, never fails but where it says so. */
#ifndef MINATO_GRAPHICS_CAMERA_RULES_H
#define MINATO_GRAPHICS_CAMERA_RULES_H

#include <stddef.h>
#include <stdint.h>

#include "APKRuntime/guest_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MINATO_CAMERA_MAX_FORMATS 96u
#define MINATO_CAMERA_MAX_PHOTO_SIZES 8u

/* Named limits (ADR-0073's risks: start at 1080p30, measure on the iPhone): the video streams listed reach at most
   1920 x 1080 pixels, the frame rates 30, the stills 12 megapixels (a 48 MP still is a quality AVFoundation gives
   only with its own prioritization), the zoom ratio 10. */
#define MINATO_CAMERA_MAX_VIDEO_PIXELS (1920 * 1080)
#define MINATO_CAMERA_MAX_FPS 30
#define MINATO_CAMERA_MAX_STILL_PIXELS (4032 * 3024)
#define MINATO_CAMERA_MAX_ZOOM_RATIO 10.0f

/* The focal length the facts give (AVFoundation tells none): with it, the sensor size follows from the format's field
   of view, so that an app's view angles come out as the iPhone's. */
#define MINATO_CAMERA_FOCAL_LENGTH_MM 4.25f

/* The exposure compensation step the facts give, 1/6 EV, and its range's bound, 2 EV. */
#define MINATO_CAMERA_BIAS_STEPS_PER_EV 6
#define MINATO_CAMERA_BIAS_MAX_STEPS 12

/* One AVCaptureDevice.Format as the rules need it. */
struct minato_camera_format {
    int32_t width, height;     // its formatDescription's dimensions
    uint8_t yuv;               // 1 for '420v' or '420f'
    float fps_max;             // the most of its videoSupportedFrameRateRanges' maxFrameRate
    float field_of_view;       // videoFieldOfView, horizontal, in degrees
    float iso_min, iso_max;
    int64_t exposure_min_ns, exposure_max_ns;
    float zoom_max;            // videoMaxZoomFactor
    uint32_t photo_count;      // supportedMaxPhotoDimensions
    int32_t photo[MINATO_CAMERA_MAX_PHOTO_SIZES][2];
};

/* One AVCaptureDevice as the rules need it. */
struct minato_camera_device {
    int32_t facing;            // 1 back, 0 front
    uint8_t autofocus;         // isFocusModeSupported(.autoFocus)
    uint8_t flash;             // hasFlash
    float aperture;            // lensAperture, the f-number
    float min_focus_mm;        // minimumFocusDistance, < 0 when unknown
    float bias_min, bias_max;  // min/maxExposureTargetBias, in EV
    float wide_factor;         // the videoZoomFactor of the wide camera's picture: a virtual device's first switch-over
                               // factor when its first camera is the ultra wide, else 1
    uint32_t format_count;
    struct minato_camera_format formats[MINATO_CAMERA_MAX_FORMATS];
};

/* The facts (guest_interface.h's list) of `device` into `out`: their bytes, also when more than `capacity` (then
   nothing past it is written). 0 for a device with no format the rules can run a stream on. */
size_t minato_camera_facts(const struct minato_camera_device *device, uint8_t *out, size_t capacity);

/* The still the facts give a JPEG stream at (the largest a YUV format takes, within MINATO_CAMERA_MAX_STILL_PIXELS)
   into width and height: 0, or -1 when there is none. */
int minato_camera_still(const struct minato_camera_device *device, int32_t *width, int32_t *height);

/* The format `streams` run on: one with the largest PRIVATE or YUV stream's size, else the smallest larger one of
   its aspect, else the smallest larger one; at `fps` (0 for MINATO_CAMERA_MAX_FPS); taking a JPEG stream's size as a
   still. Its index, or -1 when no format takes them. */
int minato_camera_choose_format(const struct minato_camera_device *device, const struct apkrt_camera_stream *streams,
                                uint32_t count, int32_t fps);

/* A camera's 3A: what a trigger started and how far it is. */
struct minato_camera_3a {
    uint8_t focus_phase, exposure_phase;  // 0 none, 1 triggered and not yet adjusting, 2 adjusting
    uint32_t focus_frames, exposure_frames;
};

/* A trigger is waited on for at most this many frames before it counts as done without the device adjusting (the
   picture was already sharp or exposed), and for at most the second while the device adjusts. */
#define MINATO_CAMERA_TRIGGER_START_FRAMES 5u
#define MINATO_CAMERA_TRIGGER_MOST_FRAMES 90u

/* A request's triggers: af_trigger (camera2's CONTROL_AF_TRIGGER_*: 1 start, 2 cancel) and ae_precapture
   (CONTROL_AE_PRECAPTURE_TRIGGER_*: 1 start, 2 cancel). */
void minato_camera_3a_trigger(struct minato_camera_3a *state, uint8_t af_trigger, uint8_t ae_precapture);

/* What the device answers at a frame. */
struct minato_camera_adjusting {
    uint8_t autofocus;                    // the device focuses at all (else it is always FOCUSED)
    uint8_t focus, exposure, white_balance;  // isAdjustingFocus, isAdjustingExposure, isAdjustingWhiteBalance
    uint8_t flash_available;              // isFlashAvailable
};

/* The frame's APKRT_CAMERA_STATE_* bits, the state moved on by one frame: from a trigger on, FOCUSING (EXPOSING)
   until the device adjusted and stopped, or did not start within MINATO_CAMERA_TRIGGER_START_FRAMES, or adjusted for
   MINATO_CAMERA_TRIGGER_MOST_FRAMES; then FOCUSED. Without a trigger, as the device adjusts. */
uint32_t minato_camera_3a_frame(struct minato_camera_3a *state, const struct minato_camera_adjusting *now);

/* A zoom ratio (within the facts' ZOOM) as the device's videoZoomFactor, within 1 and `zoom_max`. */
float minato_camera_zoom_factor(const struct minato_camera_device *device, float ratio, float zoom_max);

/* An exposure compensation in the facts' steps as the device's exposure target bias in EV, within its range. */
float minato_camera_bias(const struct minato_camera_device *device, int32_t steps);

/* A region (left, top, right, bottom, weight) in the active array of `width` x `height` as a point of interest
   (AVFoundation's 0 to 1 of the unrotated picture) into x and y: 1, or 0 for a weight of 0 (none). */
int minato_camera_point(int32_t facing, const int32_t region[5], int32_t width, int32_t height, float *x, float *y);

/* The rotation (videoRotationAngle, degrees clockwise) of the frames' connection: the facts' SENSOR_ORIENTATION of
   90 (back) and 270 (front) hold for AVFoundation's unrotated back picture and for its front picture turned by 180. */
int32_t minato_camera_frame_rotation(int32_t facing);

/* The rotation of a still's connection for camera2's JPEG_ORIENTATION. */
int32_t minato_camera_still_rotation(int32_t facing, int32_t jpeg_orientation);

#ifdef __cplusplus
}
#endif

#endif
