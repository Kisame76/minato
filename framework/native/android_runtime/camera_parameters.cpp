// The legacy Camera's parameters (camera_parameters.h): Parameters::initialize's steps in its order, each key as it
// sets or removes it, from camera2's static characteristics. Left out, as a camera of Minato's has none: recommended
// stream configurations (supportsPreferredConfigs), scene mode overrides, an external camera's FOVs and the quirks
// (none of them is set by a camera2 device).
#include "camera_parameters.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace minato::camera {
namespace {

// Parameters.h's limits.
constexpr int32_t kMaxPreviewWidth = 1920, kMaxPreviewHeight = 1920;
constexpr int32_t kMaxInitialPreviewWidth = 1920, kMaxInitialPreviewHeight = 1080;
constexpr int64_t kMaxPreviewRecordDurationNs = 100000000;  // MAX_PREVIEW_RECORD_DURATION_NS, 10 fps
constexpr int64_t kSlowJpegModeThreshold = 33400000;
constexpr int32_t kMaxDefaultFps = 30, kFpsMargin = 1, kFpsToApiScale = 1000;
constexpr unsigned kZoomSteps = 100;
constexpr float kAspectRatioTolerance = 0.01f;

// HAL pixel formats and camera2's enums.
constexpr int32_t kRgb565 = 4, kRgba8888 = 1, kYcbcr422Sp = 0x10, kYcrcb420Sp = 0x11, kYcbcr422I = 0x14,
                  kYv12 = 0x32315659, kBlob = 0x21, kPrivate = 0x22, kYcbcr420888 = 0x23;

struct Size {
    int32_t width, height;
};

/// CameraParameters: its keys in order (a KeyedVector of String8), set and removed.
class Parameters {
public:
    void set(const std::string &key, const std::string &value) { map_[key] = value; }
    void set(const std::string &key, long value) { map_[key] = std::to_string(value); }
    void set_float(const std::string &key, float value) {
        char text[32];
        std::snprintf(text, sizeof text, "%g", static_cast<double>(value));
        map_[key] = text;
    }
    void remove(const std::string &key) { map_.erase(key); }
    std::string flatten() const {
        std::string out;
        for (const auto &[key, value] : map_) out += (out.empty() ? "" : ";") + key + "=" + value;
        return out;
    }

private:
    std::map<std::string, std::string> map_;
};

std::string size_text(Size size) {
    return std::to_string(size.width) + "x" + std::to_string(size.height);
}

int64_t min_frame_duration(const StaticInfo &info, Size size, int32_t format) {
    for (const auto &duration : info.min_durations) {
        if (duration.format == format && duration.width == size.width && duration.height == size.height) {
            return duration.duration;
        }
    }
    return -1;
}

/// getFilteredSizes: the PRIVATE output sizes within the bounds' areas, not slower than 10 fps; false for none.
bool filtered_sizes(const StaticInfo &info, Size lower, Size upper, std::vector<Size> &sizes) {
    sizes.clear();
    for (const auto &stream : info.streams) {
        const int64_t area = int64_t{stream.width} * stream.height;
        if (stream.input || stream.format != kPrivate || area < int64_t{lower.width} * lower.height ||
            area > int64_t{upper.width} * upper.height) {
            continue;
        }
        if (min_frame_duration(info, {stream.width, stream.height}, kPrivate) > kMaxPreviewRecordDurationNs) continue;
        sizes.push_back({stream.width, stream.height});
    }
    return !sizes.empty();
}

/// isFpsSupported: every size reaches `fps` (with a margin of 1).
bool fps_supported(const StaticInfo &info, const std::vector<Size> &sizes, int32_t fps) {
    for (const Size &size : sizes) {
        const int64_t duration = min_frame_duration(info, size, kPrivate);
        if (duration <= 0) return false;
        const auto most = static_cast<int32_t>(1e9 / static_cast<double>(duration)) + kFpsMargin;
        if (fps > most) return false;
    }
    return true;
}

std::vector<Size> jpeg_sizes(const StaticInfo &info) {
    std::vector<Size> sizes;
    for (const auto &stream : info.streams) {
        if (!stream.input && stream.format == kBlob) sizes.push_back({stream.width, stream.height});
    }
    return sizes;
}

Size max_size(const std::vector<Size> &sizes) {
    Size most = {-1, -1};
    for (const Size &size : sizes) {
        if (size.width > most.width || (size.width == most.width && size.height > most.height)) most = size;
    }
    return most;
}

/// A list's entries by name, joined by ',' the way Parameters.cpp joins them: a mode it skips leaves the comma
/// before it, unless it is the first (its addComma flag), as Android's strings show (e.g. "auto," for a scene mode list
/// of face priority alone).
template <typename Name>
std::string joined(const std::vector<int32_t> &modes, Name name, bool comma_first = false,
                   std::string start = "") {
    std::string out = std::move(start);
    bool comma = comma_first;
    for (const int32_t mode : modes) {
        if (comma) out += ",";
        comma = true;
        const char *text = name(mode);
        if (text == nullptr) {
            comma = false;
            continue;
        }
        out += text;
    }
    return out;
}

}  // namespace

std::string legacy_parameters(const StaticInfo &info, const VideoBounds &video) {
    Parameters params;
    // buildFastInfo.
    const int32_t array_width = info.active_array[2], array_height = info.active_array[3];
    if (info.fps_ranges.size() < 2 || info.fps_ranges.size() % 2 != 0) return "";
    std::vector<Size> preview_sizes;
    if (!filtered_sizes(info, {0, 0}, {kMaxPreviewWidth, kMaxPreviewHeight}, preview_sizes)) return "";
    std::vector<std::pair<int32_t, int32_t>> preview_ranges;
    for (size_t i = 0; i < info.fps_ranges.size(); i += 2) {
        if (!fps_supported(info, preview_sizes, info.fps_ranges[i + 1]) || info.fps_ranges[i + 1] > kMaxDefaultFps) {
            continue;
        }
        preview_ranges.emplace_back(info.fps_ranges[i], info.fps_ranges[i + 1]);
    }
    if (preview_ranges.empty()) return "";
    std::pair<int32_t, int32_t> best = preview_ranges[0];
    for (size_t i = 1; i < preview_ranges.size(); i++) {
        const int32_t current = best.second - best.first, next = preview_ranges[i].second - preview_ranges[i].first;
        if (next > current || (next == current && best.first > preview_ranges[i].first)) best = preview_ranges[i];
    }
    int32_t max_faces = info.max_faces.value_or(0);
    float focal_length = info.focal_lengths.empty() ? 0 : info.focal_lengths[0];
    for (const float each : info.focal_lengths) focal_length = std::fmin(focal_length, each);
    if (!info.focal_lengths.empty() && info.preview_focal_length > 0) focal_length = info.preview_focal_length;
    std::set<int32_t> output_formats;
    for (const auto &stream : info.streams) {
        if (!stream.input) output_formats.insert(stream.format);
    }
    if (output_formats.empty()) return "";

    // initialize: preview and video sizes. Minato's bounds stand in for MediaProfiles' encoders.
    const Size video_lower = {video.min_width, video.min_height}, video_upper = {video.max_width, video.max_height};
    std::vector<Size> available_preview, available_video;
    if (!filtered_sizes(info, {0, 0}, {kMaxPreviewWidth, kMaxPreviewHeight}, available_preview)) return "";
    if (!filtered_sizes(info, video_lower, video_upper, available_video)) return "";
    Size preview = {0, 0};
    for (const Size &size : available_preview) {
        if (size.width < preview.width || size.height < preview.height || size.width > kMaxInitialPreviewWidth ||
            size.height > kMaxInitialPreviewHeight) {
            continue;
        }
        for (const Size &each : available_video) {
            if (each.width == size.width && each.height == size.height) preview = size;
        }
    }
    if (preview.width == 0) return "";
    params.set("preview-size", size_text(preview));
    params.set("video-size", size_text(preview));
    params.set("preferred-preview-size-for-video", size_text(preview));
    std::string sizes_text;
    for (const Size &size : available_preview) sizes_text += (sizes_text.empty() ? "" : ",") + size_text(size);
    params.set("preview-size-values", sizes_text);
    sizes_text.clear();
    for (const Size &size : available_video) sizes_text += (sizes_text.empty() ? "" : ",") + size_text(size);
    params.set("video-size-values", sizes_text);

    params.set("preview-format", "yuv420sp");
    {
        std::string formats;
        bool comma = false;
        for (const int32_t format : output_formats) {
            if (comma) formats += ",";
            comma = true;
            switch (format) {
                case kYcbcr422Sp: formats += "yuv422sp"; break;
                case kYcrcb420Sp: formats += "yuv420sp"; break;
                case kYcbcr422I: formats += "yuv422i-yuyv"; break;
                case kYv12: formats += "yuv420p"; break;
                case kRgb565: formats += "rgb565"; break;
                case kRgba8888: formats += "rgba8888"; break;
                case kYcbcr420888: formats += "yuv420p,yuv420sp"; break;
                default: comma = false; break;  // PRIVATE, RAW16 (0x20), BLOB and the unknown ones
            }
        }
        params.set("preview-format-values", formats);
    }
    params.set("preview-frame-rate", best.second);
    params.set("preview-fps-range", std::to_string(best.first * kFpsToApiScale) + "," +
                                            std::to_string(best.second * kFpsToApiScale));
    {
        std::string ranges, rates;
        std::set<int32_t> seen;
        for (size_t i = 0; i < info.fps_ranges.size(); i += 2) {
            if (!fps_supported(info, available_preview, info.fps_ranges[i + 1])) continue;
            ranges += (ranges.empty() ? "(" : ",(") + std::to_string(info.fps_ranges[i] * kFpsToApiScale) + "," +
                      std::to_string(info.fps_ranges[i + 1] * kFpsToApiScale) + ")";
        }
        for (size_t i = 0; i < info.fps_ranges.size(); i += 2) {
            const int32_t fps = info.fps_ranges[i + 1];
            if (!fps_supported(info, available_preview, fps) || !seen.insert(fps).second) continue;
            rates += (seen.size() > 1 ? "," : "") + std::to_string(fps);
        }
        params.set("preview-fps-range-values", ranges);
        params.set("preview-frame-rate-values", rates);
    }

    // Pictures and thumbnails.
    const std::vector<Size> jpegs = jpeg_sizes(info);
    if (jpegs.empty()) return "";
    const Size picture = jpegs[0];
    params.set("picture-size", size_text(picture));
    sizes_text.clear();
    for (const Size &size : jpegs) sizes_text += (sizes_text.empty() ? "" : ",") + size_text(size);
    params.set("picture-size-values", sizes_text);
    params.set("picture-format", "jpeg");
    params.set("picture-format-values", "jpeg");
    if (info.thumbnail_sizes.size() < 4 || info.thumbnail_sizes.size() % 2 != 0) return "";
    {
        const float ratio = static_cast<float>(picture.width) / static_cast<float>(picture.height);
        Size thumbnail = {0, 0};
        for (size_t i = 0; i < info.thumbnail_sizes.size(); i += 2) {
            const int32_t width = info.thumbnail_sizes[i], height = info.thumbnail_sizes[i + 1];
            if (width <= 0 || height <= 0) continue;
            const float current = static_cast<float>(width) / static_cast<float>(height);
            if (std::fabs(current - ratio) < kAspectRatioTolerance && thumbnail.width < width) {
                thumbnail = {width, height};
            }
        }
        if (thumbnail.width == 0 || thumbnail.height == 0) {
            thumbnail = {info.thumbnail_sizes[0], info.thumbnail_sizes[1]};
        }
        params.set("jpeg-thumbnail-width", thumbnail.width);
        params.set("jpeg-thumbnail-height", thumbnail.height);
        std::string thumbnails;
        for (size_t i = 0; i < info.thumbnail_sizes.size(); i += 2) {
            thumbnails += (i == 0 ? "" : ",") + size_text({info.thumbnail_sizes[i], info.thumbnail_sizes[i + 1]});
        }
        params.set("jpeg-thumbnail-size-values", thumbnails);
    }
    params.set("jpeg-thumbnail-quality", 90);
    params.set("jpeg-quality", 90);
    params.set("rotation", 0);

    // White balance, effects, antibanding, scene modes.
    params.set("whitebalance", "auto");
    if (!info.awb_modes || info.awb_modes->empty()) {
        params.set("whitebalance-values", "auto");
    } else {
        params.set("whitebalance-values", joined(*info.awb_modes, [](int32_t mode) -> const char * {
                       static const char *const names[] = {nullptr,  "auto",          "incandescent", "fluorescent",
                                                           "warm-fluorescent", "daylight", "cloudy-daylight",
                                                           "twilight", "shade"};
                       return mode >= 0 && mode <= 8 ? names[mode] : nullptr;
                   }));
    }
    params.set("effect", "none");
    if (!info.effects || info.effects->empty()) {
        params.set("effect-values", "none");
    } else {
        params.set("effect-values", joined(*info.effects, [](int32_t mode) -> const char * {
                       static const char *const names[] = {"none",      "mono",       "negative",
                                                           "solarize",  "sepia",      "posterize",
                                                           "whiteboard", "blackboard", "aqua"};
                       return mode >= 0 && mode <= 8 ? names[mode] : nullptr;
                   }));
    }
    params.set("antibanding", "auto");
    if (!info.antibanding_modes || info.antibanding_modes->empty()) {
        params.set("antibanding-values", "off");
    } else {
        params.set("antibanding-values", joined(*info.antibanding_modes, [](int32_t mode) -> const char * {
                       static const char *const names[] = {"off", "50hz", "60hz", "auto"};
                       return mode >= 0 && mode <= 3 ? names[mode] : nullptr;
                   }));
    }
    params.set("scene-mode", "auto");
    if (!info.scene_modes || info.scene_modes->empty()) {
        params.remove("scene-mode");
    } else {
        bool none = false;
        const std::string modes = joined(
                *info.scene_modes,
                [&none](int32_t mode) -> const char * {
                    static const char *const names[] = {nullptr,       nullptr,     "action",   "portrait",
                                                        "landscape",   "night",     "night-portrait", "theatre",
                                                        "beach",       "snow",      "sunset",   "steadyphoto",
                                                        "fireworks",   "sports",    "party",    "candlelight",
                                                        "barcode",     nullptr,     "hdr"};
                    if (mode == 0) none = true;  // DISABLED
                    return mode >= 0 && mode <= 18 ? names[mode] : nullptr;
                },
                true, "auto");
        if (!none) {
            params.set("scene-mode-values", modes);
        } else {
            params.remove("scene-mode");
        }
    }

    // Flash.
    if (info.flash_available.value_or(false)) {
        params.set("flash-mode", "off");
        std::string modes = "off,auto,on,torch";
        for (const int32_t mode : info.ae_modes.value_or(std::vector<int32_t>{})) {
            if (mode == 4) {  // ON_AUTO_FLASH_REDEYE
                modes += ",red-eye";
                break;
            }
        }
        params.set("flash-mode-values", modes);
    } else {
        params.remove("flash-mode");
        params.remove("flash-mode-values");
    }

    // Focus.
    const bool fixed = !info.min_focus_distance || *info.min_focus_distance == 0;
    if (fixed) {
        params.set("focus-mode", "fixed");
        params.set("focus-mode-values", "fixed");
    } else {
        params.set("focus-mode", "auto");
        const bool infinity = info.focus_calibration && *info.focus_calibration != 0;  // not UNCALIBRATED
        params.set("focus-mode-values",
                   joined(info.af_modes.value_or(std::vector<int32_t>{}),
                          [](int32_t mode) -> const char * {
                              static const char *const names[] = {nullptr, "auto", "macro", "continuous-video",
                                                                  "continuous-picture", "edof"};
                              return mode >= 0 && mode <= 5 ? names[mode] : nullptr;
                          },
                          infinity, infinity ? "infinity" : ""));
    }
    params.set("max-num-focus-areas", fixed ? 0 : info.max_regions[2]);
    params.set("focus-areas", "(0,0,0,0,0)");
    params.set_float("focal-length", focal_length);
    {
        // calculatePictureFovs, without the crop-region quirk: the still's aspect against the active array's.
        if (info.pixel_array[0] <= 0 || info.pixel_array[1] <= 0 || focal_length <= 0) return "";
        const float array_aspect = static_cast<float>(array_width) / static_cast<float>(array_height);
        const float still_aspect = static_cast<float>(picture.width) / static_cast<float>(picture.height);
        float horizontal = array_aspect > still_aspect ? still_aspect / array_aspect : 1.f;
        float vertical = array_aspect < still_aspect ? array_aspect / still_aspect : 1.f;
        horizontal *= static_cast<float>(array_width) / static_cast<float>(info.pixel_array[0]);
        vertical *= static_cast<float>(array_height) / static_cast<float>(info.pixel_array[1]);
        const auto fov = [&](float crop, float side) {
            return static_cast<float>(180 / M_PI * 2 * std::atan(crop * side / (2 * focal_length)));
        };
        params.set_float("horizontal-view-angle", fov(horizontal, info.physical_size[0]));
        params.set_float("vertical-view-angle", fov(vertical, info.physical_size[1]));
    }

    // Exposure, locks, metering.
    params.set("exposure-compensation", 0);
    if (info.ae_compensation_step[1] == 0) return "";
    params.set("max-exposure-compensation", info.ae_compensation_range[1]);
    params.set("min-exposure-compensation", info.ae_compensation_range[0]);
    params.set_float("exposure-compensation-step", static_cast<float>(info.ae_compensation_step[0]) /
                                                           static_cast<float>(info.ae_compensation_step[1]));
    if (info.ae_lock_available.value_or(false)) {
        params.set("auto-exposure-lock", "false");
        params.set("auto-exposure-lock-supported", "true");
    } else {
        params.set("auto-exposure-lock-supported", "false");
    }
    if (info.awb_lock_available.value_or(false)) {
        params.set("auto-whitebalance-lock", "false");
        params.set("auto-whitebalance-lock-supported", "true");
    } else {
        params.set("auto-whitebalance-lock-supported", "false");
    }
    params.set("max-num-metering-areas", info.max_regions[0]);
    params.set("metering-areas", "(0,0,0,0,0)");

    // Zoom: 100 steps from 1 to the largest digital zoom, in hundredths, without repeats.
    if (std::fabs(info.max_digital_zoom - 1.f) > 0.00001f) {
        params.set("zoom", 0);
        std::string ratios;
        float zoom = 1.f;
        const float increment = (info.max_digital_zoom - zoom) / static_cast<float>(kZoomSteps - 1);
        int previous = -1;
        size_t steps = 0;
        for (unsigned i = 0; i < kZoomSteps; i++) {
            const int current = static_cast<int>(zoom * 100);
            if (current != previous) {
                ratios += (steps == 0 ? "" : ",") + std::to_string(current);
                previous = current;
                steps++;
            }
            zoom += increment;
        }
        if (steps > 0) {
            params.set("zoom-ratios", ratios);
            params.set("zoom-supported", "true");
            params.set("max-zoom", static_cast<long>(steps - 1));
        } else {
            params.set("zoom-supported", "false");
        }
    } else {
        params.set("zoom-supported", "false");
    }
    params.set("smooth-zoom-supported", "false");
    params.set("focus-distances", "Infinity,Infinity,Infinity");
    params.set("max-num-detected-faces-hw", max_faces);
    params.set("max-num-detected-faces-sw", 0);
    params.set("video-frame-format", "android-opaque");
    params.set("recording-hint", "false");
    params.set("video-snapshot-supported", "true");
    params.set("video-stabilization", "false");
    params.set("video-stabilization-supported",
               info.video_stabilization_modes && info.video_stabilization_modes->size() > 1 ? "true" : "false");
    // A JPEG slower than 33.4 ms at its largest size: slow JPEG mode, no video snapshot.
    if (min_frame_duration(info, max_size(jpegs), kBlob) > kSlowJpegModeThreshold) {
        params.set("video-snapshot-supported", "false");
    }
    return params.flatten();
}

namespace {

/// A flattened string's entries in their order.
std::vector<std::pair<std::string, std::string>> unflatten(const std::string &flattened) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t at = 0;
    while (at < flattened.size()) {
        size_t end = flattened.find(';', at);
        if (end == std::string::npos) end = flattened.size();
        const std::string entry = flattened.substr(at, end - at);
        const size_t equals = entry.find('=');
        if (equals != std::string::npos) out.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
        at = end + 1;
    }
    return out;
}

const std::string *find(const std::vector<std::pair<std::string, std::string>> &entries, const std::string &key) {
    const std::string *found = nullptr;
    for (const auto &[k, v] : entries) {
        if (k == key) found = &v;  // the last one, as CameraParameters2's set keeps one per key
    }
    return found;
}

int position(const std::vector<std::pair<std::string, std::string>> &entries, const std::string &key) {
    int found = -1;
    for (size_t n = 0; n < entries.size(); n++) {
        if (entries[n].first == key) found = static_cast<int>(n);
    }
    return found;
}

bool listed(const std::string *value, const std::string *list) {
    if (value == nullptr || list == nullptr) return false;
    size_t at = 0;
    while (at <= list->size()) {
        size_t end = list->find(',', at);
        if (end == std::string::npos) end = list->size();
        if (list->compare(at, end - at, *value) == 0) return true;
        at = end + 1;
    }
    return false;
}

/// An integer of the whole text, or false.
bool integer(const std::string *text, long *value) {
    if (text == nullptr || text->empty()) return false;
    char *end = nullptr;
    *value = std::strtol(text->c_str(), &end, 10);
    return *end == '\0';
}

}  // namespace

bool accept_parameters(const std::string &initial, const std::string &asked) {
    const auto base = unflatten(initial), next = unflatten(asked);
    const auto same_or_listed = [&](const char *key, const char *values) {
        const std::string *value = find(next, key), *was = find(base, key);
        if (value == nullptr || (was != nullptr && *value == *was)) return true;
        return listed(value, find(base, values));
    };
    // PREVIEW_SIZE.
    if (!same_or_listed("preview-size", "preview-size-values")) return false;
    // PREVIEW_FPS_RANGE or PREVIEW_FRAME_RATE, whichever was set last; one of them must be there.
    const std::string *range = find(next, "preview-fps-range"), *single = find(next, "preview-frame-rate");
    if (range == nullptr && single == nullptr) return false;
    const bool use_single =
            range == nullptr ||
            (single != nullptr && position(next, "preview-frame-rate") > position(next, "preview-fps-range"));
    const std::string *ranges = find(base, "preview-fps-range-values");
    if (!use_single) {
        if (ranges == nullptr || ranges->find("(" + *range + ")") == std::string::npos) return false;
    } else {
        long fps = 0;
        if (!integer(single, &fps) || ranges == nullptr) return false;
        bool within = false;
        for (size_t at = ranges->find('('); at != std::string::npos; at = ranges->find('(', at + 1)) {
            long low = 0, high = 0;
            if (std::sscanf(ranges->c_str() + at, "(%ld,%ld)", &low, &high) == 2) {
                within |= low / 1000 <= fps && fps <= high / 1000;
            }
        }
        if (!within) return false;
    }
    // PREVIEW_FORMAT: NV21 and YV12 always (a camera of flexible YUV), else the camera's list.
    if (const std::string *format = find(next, "preview-format");
        format != nullptr && *format != "yuv420sp" && *format != "yuv420p" &&
        !listed(format, find(base, "preview-format-values"))) {
        return false;
    }
    // PICTURE_SIZE, the qualities, ROTATION.
    if (!same_or_listed("picture-size", "picture-size-values")) return false;
    for (const char *key : {"jpeg-thumbnail-quality", "jpeg-quality"}) {
        long quality = 0;
        if (find(next, key) != nullptr && (!integer(find(next, key), &quality) || quality < 1 || quality > 100)) {
            return false;
        }
    }
    long rotation = 0;
    if (find(next, "rotation") != nullptr &&
        (!integer(find(next, "rotation"), &rotation) || rotation < 0 || rotation % 90 != 0 || rotation > 270)) {
        return false;
    }
    // The modes, each one of its list.
    for (const auto &[key, values] : std::vector<std::pair<const char *, const char *>>{
                 {"effect", "effect-values"},             {"antibanding", "antibanding-values"},
                 {"scene-mode", "scene-mode-values"},     {"flash-mode", "flash-mode-values"},
                 {"whitebalance", "whitebalance-values"}, {"focus-mode", "focus-mode-values"}}) {
        if (!same_or_listed(key, values)) return false;
    }
    // EXPOSURE_COMPENSATION and ZOOM within their ranges.
    long value = 0, low = 0, high = 0;
    if (find(next, "exposure-compensation") != nullptr &&
        (!integer(find(next, "exposure-compensation"), &value) ||
         !integer(find(base, "min-exposure-compensation"), &low) ||
         !integer(find(base, "max-exposure-compensation"), &high) || value < low || value > high)) {
        return false;
    }
    if (find(next, "zoom") != nullptr &&
        (!integer(find(next, "zoom"), &value) || !integer(find(base, "max-zoom"), &high) || value < 0 ||
         value > high)) {
        return false;
    }
    // VIDEO_SIZE.
    return same_or_listed("video-size", "video-size-values");
}

// The facts' keys and types (guest_interface.h's apkrt_camera_key, APKRT_CAMERA_TYPE_*), the formats of its streams.
enum : uint32_t {
    kFacing = 1, kOrientation, kPixelArray, kActiveArray, kPhysicalSize, kFocalLengths, kApertures, kMinFocusDistance,
    kFlash, kAeCompensation, kZoom, kFpsRanges, kIso, kExposureNs, kAfModes, kAwbModes, kFaces, kStreams, kStalls,
};
enum : uint32_t { kI32 = 1, kI64 = 2, kF32 = 3 };
constexpr int64_t kFactYuv = 0x23, kFactJpeg = 0x100;

bool read_facts(const unsigned char *data, size_t size, Facts *facts) {
    Facts out;
    StaticInfo &info = out.info;
    bool facing = false, orientation = false, pixel = false, active = false, flash = false, autofocus = false;
    std::vector<int32_t> af_modes;
    for (size_t at = 0; at < size;) {
        if (size - at < 12) return false;
        uint32_t header[3];
        std::memcpy(header, data + at, sizeof header);
        const uint32_t key = header[0], type = header[1], count = header[2];
        const size_t bytes = type == kI64 ? 8 : 4;
        if (type != kI32 && type != kI64 && type != kF32) return false;
        const uint64_t length = 12 + uint64_t{count} * bytes, padded = (length + 7) & ~uint64_t{7};
        if (padded > size - at) return false;
        const unsigned char *values = data + at + 12;
        const auto i32 = [&](uint32_t n) {
            int32_t value;
            std::memcpy(&value, values + 4 * size_t{n}, 4);
            return value;
        };
        const auto i64 = [&](uint32_t n) {
            int64_t value;
            std::memcpy(&value, values + 8 * size_t{n}, 8);
            return value;
        };
        const auto f32 = [&](uint32_t n) {
            float value;
            std::memcpy(&value, values + 4 * size_t{n}, 4);
            return value;
        };
        const bool i32s = type == kI32, f32s = type == kF32, i64s = type == kI64;
        switch (key) {
            case kFacing: if (i32s && count == 1) out.facing = i32(0), facing = true; break;
            case kOrientation: if (i32s && count == 1) out.orientation = i32(0), orientation = true; break;
            case kPixelArray:
                if (i32s && count == 2) info.pixel_array = {i32(0), i32(1)}, pixel = true;
                break;
            case kActiveArray:
                // left, top, right, bottom (exclusive) as camera2's xmin, ymin, width, height.
                if (i32s && count == 4) info.active_array = {i32(0), i32(1), i32(2) - i32(0), i32(3) - i32(1)};
                active = i32s && count == 4;
                break;
            case kPhysicalSize: if (f32s && count == 2) info.physical_size = {f32(0), f32(1)}; break;
            case kFocalLengths:
                if (f32s) {
                    for (uint32_t n = 0; n < count; n++) info.focal_lengths.push_back(f32(n));
                }
                break;
            case kMinFocusDistance:
                if (f32s && count == 1) info.min_focus_distance = f32(0), autofocus = f32(0) > 0;
                break;
            case kFlash: if (i32s && count == 1) flash = i32(0) != 0; break;
            case kAeCompensation:
                if (i32s && count == 4) {
                    info.ae_compensation_range = {i32(0), i32(1)};
                    info.ae_compensation_step = {i32(2), i32(3)};
                }
                break;
            case kZoom: if (f32s && count == 2) info.max_digital_zoom = f32(1); break;
            case kFpsRanges:
                if (i32s) {
                    for (uint32_t n = 0; n + 1 < count; n += 2) {
                        info.fps_ranges.push_back(i32(n));
                        info.fps_ranges.push_back(i32(n + 1));
                    }
                }
                break;
            case kAfModes:
                if (i32s) {
                    for (uint32_t n = 0; n < count; n++) af_modes.push_back(i32(n));
                }
                break;
            case kAwbModes:
                if (i32s) {
                    info.awb_modes.emplace();
                    for (uint32_t n = 0; n < count; n++) info.awb_modes->push_back(i32(n));
                }
                break;
            case kFaces: if (i32s && count == 1) info.max_faces = i32(0); break;
            case kStreams:
                if (i64s && count % 4 == 0) {
                    for (uint32_t n = 0; n < count; n += 4) {
                        const int64_t format = i64(n), width = i64(n + 1), height = i64(n + 2), duration = i64(n + 3);
                        if (width <= 0 || height <= 0 || width > INT32_MAX || height > INT32_MAX) return false;
                        const auto w = static_cast<int32_t>(width), h = static_cast<int32_t>(height);
                        // A YUV size is camera2's PRIVATE and YUV_420_888 stream, a JPEG its BLOB.
                        std::vector<int32_t> formats;
                        if (format == kFactYuv) formats = {kPrivate, kYcbcr420888};
                        if (format == kFactJpeg) formats = {kBlob};
                        for (const int32_t hal : formats) {
                            info.streams.push_back({hal, w, h, false});
                            info.min_durations.push_back({hal, w, h, duration});
                        }
                    }
                }
                break;
            default: break;
        }
        at += static_cast<size_t>(padded);
    }
    if (!facing || !orientation || !pixel || !active || info.streams.empty() || info.fps_ranges.empty()) return false;
    // A LIMITED camera with BACKWARD_COMPATIBLE (ADR-0073 decision 1), what the facts do not say set as Minato's:
    info.hardware_level = 0;                    // LIMITED
    info.focus_calibration = 0;                 // UNCALIBRATED: the host's lens position has no unit
    info.preview_focal_length = info.focal_lengths.empty() ? 0 : info.focal_lengths[0];
    for (const float each : info.focal_lengths) info.preview_focal_length = std::fmin(info.preview_focal_length, each);
    info.af_modes = autofocus ? af_modes : std::vector<int32_t>{0};
    info.ae_modes = flash ? std::vector<int32_t>{1, 2, 3} : std::vector<int32_t>{1};  // ON, ON_AUTO_FLASH, ALWAYS
    info.flash_available = flash;
    info.effects = std::vector<int32_t>{0};
    info.antibanding_modes = std::vector<int32_t>{0, 3};    // OFF, AUTO (iOS avoids flicker by itself)
    info.scene_modes = std::vector<int32_t>{0};             // DISABLED: none
    info.face_detect_modes = info.max_faces.value_or(0) > 0 ? std::vector<int32_t>{0, 1} : std::vector<int32_t>{0};
    info.video_stabilization_modes = std::vector<int32_t>{0};
    info.max_regions = {1, 0, autofocus ? 1 : 0};           // AE, AWB, AF
    info.ae_lock_available = true;
    info.awb_lock_available = true;
    info.thumbnail_sizes = {0, 0, 160, 120, 320, 240};
    if (!info.awb_modes) info.awb_modes = std::vector<int32_t>{1};
    *facts = std::move(out);
    return true;
}

}  // namespace minato::camera
