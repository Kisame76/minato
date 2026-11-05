// The legacy android.hardware.Camera's parameters (ADR-0073 step e): Camera.Parameters as Android 15's cameraservice
// makes them for a camera (frameworks/av api1/client2/Parameters.cpp's initialize at android-15.0.0_r1), from the
// camera's static characteristics in camera2's terms, which Minato makes from the host's facts. Plain C++, so that
// its test runs on the host.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace minato::camera {

/// A camera's static characteristics as camera2's metadata has them (ANDROID_* tags), HAL formats and all; an
/// optional entry is one a camera may lack.
struct StaticInfo {
    struct Stream {
        int32_t format, width, height;
        bool input;
    };
    struct Duration {
        int64_t format, width, height, duration;
    };
    int32_t hardware_level = 0;                     // INFO_SUPPORTED_HARDWARE_LEVEL
    std::array<int32_t, 4> active_array{};          // SENSOR_INFO_ACTIVE_ARRAY_SIZE: xmin, ymin, width, height
    std::array<int32_t, 2> pixel_array{};           // SENSOR_INFO_PIXEL_ARRAY_SIZE
    std::array<float, 2> physical_size{};           // SENSOR_INFO_PHYSICAL_SIZE, millimetres
    std::vector<float> focal_lengths;               // LENS_INFO_AVAILABLE_FOCAL_LENGTHS
    float preview_focal_length = 0;                 // the PREVIEW template's LENS_FOCAL_LENGTH
    std::optional<float> min_focus_distance;        // LENS_INFO_MINIMUM_FOCUS_DISTANCE
    std::optional<int32_t> focus_calibration;       // LENS_INFO_FOCUS_DISTANCE_CALIBRATION
    std::vector<Stream> streams;                    // SCALER_AVAILABLE_STREAM_CONFIGURATIONS
    std::vector<Duration> min_durations;            // SCALER_AVAILABLE_MIN_FRAME_DURATIONS
    std::vector<int32_t> fps_ranges;                // CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, pairs
    std::optional<std::vector<int32_t>> awb_modes, effects, antibanding_modes, scene_modes, af_modes, ae_modes,
            face_detect_modes, video_stabilization_modes;
    std::optional<int32_t> max_faces;               // STATISTICS_INFO_MAX_FACE_COUNT
    std::optional<bool> flash_available;            // FLASH_INFO_AVAILABLE
    std::array<int32_t, 3> max_regions{};           // CONTROL_MAX_REGIONS: AE, AWB, AF
    std::array<int32_t, 2> ae_compensation_range{};  // CONTROL_AE_COMPENSATION_RANGE
    std::array<int32_t, 2> ae_compensation_step{0, 1};  // CONTROL_AE_COMPENSATION_STEP, a rational
    std::optional<bool> ae_lock_available, awb_lock_available;
    float max_digital_zoom = 1;                     // SCALER_AVAILABLE_MAX_DIGITAL_ZOOM
    std::vector<int32_t> thumbnail_sizes;           // JPEG_AVAILABLE_THUMBNAIL_SIZES, pairs
};

/// The video sizes' bounds, MediaProfiles' video encoders' least and largest sizes (enc.vid.width/height.min/max).
struct VideoBounds {
    int32_t min_width, min_height, max_width, max_height;
};

/// Minato's bounds for the video sizes, in place of the encoders Minato does not have yet (a named limit: Android's
/// come from MediaProfiles' H.264 encoder): 176 x 144 to 1920 x 1080.
inline constexpr VideoBounds kVideoBounds = {176, 144, 1920, 1080};

/// A camera of the host's (guest_interface.h's camera facts) in camera2's terms: its facing and orientation, and the
/// static characteristics of a LIMITED camera with BACKWARD_COMPATIBLE (ADR-0073 decision 1).
struct Facts {
    int32_t facing = 0, orientation = 0;  // camera2's LENS_FACING (0 front, 1 back) and SENSOR_ORIENTATION
    StaticInfo info;
};

/// The host's facts list read into `facts`: false for one that does not hold (an entry past the end) or lacks what
/// a camera needs (facing, orientation, the arrays, a stream, a frame rate).
bool read_facts(const unsigned char *data, size_t size, Facts *facts);

/// The camera's initial parameters, flattened as CameraParameters::flatten does (each key=value, in the keys'
/// order, joined by ';'); empty where Parameters::initialize fails (no preview size, no frame rate, no JPEG size, no
/// thumbnail size, no compensation range or step).
std::string legacy_parameters(const StaticInfo &info, const VideoBounds &video);

/// Whether Parameters::set takes `asked` for a camera whose initial parameters are `initial` (each value one of the
/// camera's lists or within its ranges: the preview, picture and video sizes, the frame rate range, the preview
/// format, the JPEG and thumbnail qualities, the rotation, the effect, antibanding, scene, flash, white balance and
/// focus modes, the exposure compensation and the zoom); a camera then gives `asked` back as its parameters, as
/// CameraParameters2 keeps them.
/// While the preview runs (`previewing`), its size and format must stay those in force (`initial` then holds them),
/// as Parameters::set refuses another one in its PREVIEW state.
bool accept_parameters(const std::string &initial, const std::string &asked, bool previewing = false);

}  // namespace minato::camera
