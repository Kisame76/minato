// ADR-0073 step e: the legacy android.hardware.Camera's parameters
// (framework/native/android_runtime/camera_parameters.cpp), on the host. Android 15's emulator's own camera, its
// static characteristics as camera-characteristics-test-granted dumped them, must give the emulator's own
// Camera.Parameters, key by key and value by value, as cameraservice's Parameters::initialize builds them
// (frameworks/av api1/client2/Parameters.cpp at android-15.0.0_r1); then the cases its branches take for other
// cameras (no flash, fixed focus, no digital zoom, no scene modes, a slow JPEG), and the host's facts read as a
// LIMITED camera's characteristics (ADR-0073 decision 1), as the model camera's give them.
#include "camera_parameters.h"

#include "check.h"

#include <array>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

namespace camera = minato::camera;

// HAL pixel formats.
constexpr int32_t kRgba8888 = 1, kRaw16 = 0x20, kBlob = 0x21, kPrivate = 0x22, kYuv420888 = 0x23, kP010 = 0x36;

/// apk-runner-35's back camera (its legacy ID 0, camera2's "1"), from camera-characteristics-test-granted's dump.
camera::StaticInfo emulator() {
    camera::StaticInfo info;
    info.hardware_level = 3;  // FULL
    info.active_array = {0, 0, 1856, 1392};
    info.pixel_array = {1856, 1392};
    info.physical_size = {3.2f, 2.4f};
    info.focal_lengths = {3.3f};
    info.preview_focal_length = 3.3f;
    info.min_focus_distance = {20.0f};
    info.focus_calibration = {1};  // APPROXIMATE
    const std::vector<std::pair<int32_t, std::vector<std::pair<int32_t, int32_t>>>> outputs = {
        {kRgba8888, {{1600, 1200}}},
        {kRaw16, {{1856, 1392}}},
        {kBlob, {{1856, 1392}, {1280, 720}, {160, 120}, {320, 240}, {640, 480}, {176, 144}, {1024, 768}}},
        {kPrivate, {{160, 120}, {320, 240}, {640, 480}, {1280, 720}, {1856, 1392}, {176, 144}, {1024, 768}}},
        {kYuv420888, {{160, 120}, {320, 240}, {640, 480}, {1280, 720}, {1856, 1392}, {176, 144}, {1024, 768}}},
        {kP010, {{160, 120}, {320, 240}, {640, 480}, {1280, 720}, {1856, 1392}, {176, 144}, {1024, 768}}},
    };
    for (const auto &[format, sizes] : outputs) {
        for (const auto &[width, height] : sizes) {
            info.streams.push_back({format, width, height, false});
            info.min_durations.push_back({format, width, height, 33331760});
        }
    }
    info.streams.push_back({kPrivate, 1856, 1392, true});
    info.streams.push_back({kYuv420888, 1856, 1392, true});
    info.fps_ranges = {15, 15, 5, 30, 15, 30, 30, 30};
    info.awb_modes = {{0, 1, 2, 3, 5, 8}};
    info.effects = {{0}};
    info.antibanding_modes = {{0, 3}};
    info.scene_modes = {{1}};
    info.af_modes = {{0, 1, 2, 3, 4}};
    info.ae_modes = {{0, 1, 2, 3}};
    info.face_detect_modes = {{0, 1, 2}};
    info.max_faces = {8};
    info.video_stabilization_modes = {{0, 1, 2}};
    info.flash_available = {true};
    info.max_regions = {1, 0, 1};  // AE, AWB, AF
    info.ae_compensation_range = {-9, 9};
    info.ae_compensation_step = {1, 3};
    info.ae_lock_available = {true};
    info.awb_lock_available = {true};
    info.max_digital_zoom = 10.0f;
    info.thumbnail_sizes = {0, 0, 160, 120, 320, 240};
    return info;
}

/// The emulator's H.264 encoder limits (MediaProfiles' enc.vid.* of its media_profiles.xml).
const camera::VideoBounds emulator_video = {176, 144, 1280, 720};

std::map<std::string, std::string> entries(const std::string &flattened) {
    std::map<std::string, std::string> out;
    std::string::size_type at = 0;
    while (at <= flattened.size() && !flattened.empty()) {
        const auto end = flattened.find(';', at);
        const std::string entry = flattened.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const auto equals = entry.find('=');
        out[entry.substr(0, equals)] = equals == std::string::npos ? "" : entry.substr(equals + 1);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return out;
}

TEST(the_emulators_camera_gives_the_emulators_parameters) {
    const char *const expected[] = {
        "antibanding-values=off,auto",
        "antibanding=auto",
        "auto-exposure-lock-supported=true",
        "auto-exposure-lock=false",
        "auto-whitebalance-lock-supported=true",
        "auto-whitebalance-lock=false",
        "effect-values=none",
        "effect=none",
        "exposure-compensation-step=0.333333",
        "exposure-compensation=0",
        "flash-mode-values=off,auto,on,torch",
        "flash-mode=off",
        "focal-length=3.3",
        "focus-areas=(0,0,0,0,0)",
        "focus-distances=Infinity,Infinity,Infinity",
        "focus-mode-values=infinity,auto,macro,continuous-video,continuous-picture",
        "focus-mode=auto",
        "horizontal-view-angle=51.7327",
        "jpeg-quality=90",
        "jpeg-thumbnail-height=240",
        "jpeg-thumbnail-quality=90",
        "jpeg-thumbnail-size-values=0x0,160x120,320x240",
        "jpeg-thumbnail-width=320",
        "max-exposure-compensation=9",
        "max-num-detected-faces-hw=8",
        "max-num-detected-faces-sw=0",
        "max-num-focus-areas=1",
        "max-num-metering-areas=1",
        "max-zoom=99",
        "metering-areas=(0,0,0,0,0)",
        "min-exposure-compensation=-9",
        "picture-format-values=jpeg",
        "picture-format=jpeg",
        "picture-size-values=1856x1392,1280x720,160x120,320x240,640x480,176x144,1024x768",
        "picture-size=1856x1392",
        "preferred-preview-size-for-video=1280x720",
        "preview-format-values=rgba8888,yuv420p,yuv420sp,",
        "preview-format=yuv420sp",
        "preview-fps-range-values=(15000,15000),(5000,30000),(15000,30000),(30000,30000)",
        "preview-fps-range=5000,30000",
        "preview-frame-rate-values=15,30",
        "preview-frame-rate=30",
        "preview-size-values=160x120,320x240,640x480,1280x720,1856x1392,176x144,1024x768",
        "preview-size=1280x720",
        "recording-hint=false",
        "rotation=0",
        "scene-mode-values=auto,",
        "scene-mode=auto",
        "smooth-zoom-supported=false",
        "vertical-view-angle=39.9662",
        "video-frame-format=android-opaque",
        "video-size-values=320x240,640x480,1280x720,176x144,1024x768",
        "video-size=1280x720",
        "video-snapshot-supported=true",
        "video-stabilization-supported=true",
        "video-stabilization=false",
        "whitebalance-values=auto,incandescent,fluorescent,daylight,shade",
        "whitebalance=auto",
        "zoom-ratios=100,109,118,127,136,145,154,163,172,181,190,200,209,218,227,236,245,254,263,272,281,290"
        ",299,309,318,327,336,345,354,363,372,381,390,399,409,418,427,436,445,454,463,472,481,490,499,509,518"
        ",527,536,545,554,563,572,581,590,599,609,618,627,636,645,654,663,672,681,690,699,709,718,727,736,745"
        ",754,763,772,781,790,799,809,818,827,836,845,854,863,872,881,890,899,909,918,927,936,945,954,963,972"
        ",981,990,999",
        "zoom-supported=true",
        "zoom=0",
    };
    const std::string flattened = camera::legacy_parameters(emulator(), emulator_video);
    const auto got = entries(flattened);
    CHECK(got.size() == sizeof expected / sizeof *expected);
    for (const char *entry : expected) {
        const std::string text = entry;
        const auto equals = text.find('=');
        const auto found = got.find(text.substr(0, equals));
        CHECK(found != got.end());
        if (found == got.end()) {
            std::printf("missing %s\n", entry);
        } else if (found->second != text.substr(equals + 1)) {
            std::printf("%s: got %s, Android %s\n", found->first.c_str(), found->second.c_str(),
                        text.substr(equals + 1).c_str());
            CHECK(false);
        }
    }
    // CameraParameters::flatten: its keys in order, each "key=value", joined by ';'.
    std::vector<std::string> keys;
    for (const auto &[key, value] : got) keys.push_back(key);
    std::string again;
    for (const auto &key : keys) again += (again.empty() ? "" : ";") + key + "=" + got.at(key);
    CHECK(again == flattened);
}

TEST(without_a_flash_the_flash_keys_go) {
    camera::StaticInfo info = emulator();
    info.flash_available = {false};
    auto got = entries(camera::legacy_parameters(info, emulator_video));
    CHECK(got.count("flash-mode") == 0 && got.count("flash-mode-values") == 0);
    info.flash_available = {true};
    info.ae_modes = {{0, 1, 2, 3, 4}};  // ON_AUTO_FLASH_REDEYE
    CHECK(entries(camera::legacy_parameters(info, emulator_video))["flash-mode-values"] == "off,auto,on,torch,red-eye");
}

TEST(a_fixed_lens_has_fixed_focus_and_no_focus_areas) {
    camera::StaticInfo info = emulator();
    info.min_focus_distance = {0.0f};
    auto got = entries(camera::legacy_parameters(info, emulator_video));
    CHECK(got["focus-mode"] == "fixed" && got["focus-mode-values"] == "fixed" && got["max-num-focus-areas"] == "0");
    info = emulator();
    info.focus_calibration = {0};  // UNCALIBRATED: no infinity
    got = entries(camera::legacy_parameters(info, emulator_video));
    CHECK(got["focus-mode-values"] == "auto,macro,continuous-video,continuous-picture");
}

TEST(no_digital_zoom_no_scene_modes_and_one_stabilization_mode) {
    camera::StaticInfo info = emulator();
    info.max_digital_zoom = 1.0f;
    info.scene_modes = {{0}};  // DISABLED
    info.video_stabilization_modes = {{0}};
    auto got = entries(camera::legacy_parameters(info, emulator_video));
    CHECK(got["zoom-supported"] == "false" && got.count("zoom-ratios") == 0 && got.count("max-zoom") == 0);
    CHECK(got.count("scene-mode") == 0 && got.count("scene-mode-values") == 0);
    CHECK(got["video-stabilization-supported"] == "false");
}

TEST(a_slow_jpeg_has_no_video_snapshot) {
    camera::StaticInfo info = emulator();
    for (auto &duration : info.min_durations) {
        if (duration.format == kBlob && duration.width == 1856) duration.duration = 50000000;
    }
    CHECK(entries(camera::legacy_parameters(info, emulator_video))["video-snapshot-supported"] == "false");
}

TEST(a_camera_without_a_preview_size_has_no_parameters) {
    camera::StaticInfo info = emulator();
    const camera::VideoBounds none = {0, 0, 0, 0};  // no encoder: no video size, no initial preview size
    CHECK(camera::legacy_parameters(info, none).empty());
    info.fps_ranges.clear();
    CHECK(camera::legacy_parameters(info, emulator_video).empty());
}

/// A facts list as guest_interface.h has it, as camera_model.m's back camera gives it.
struct FactsList {
    std::vector<unsigned char> bytes;
    template <typename T>
    FactsList &add(uint32_t key, uint32_t type, std::vector<T> values) {
        const uint32_t header[3] = {key, type, static_cast<uint32_t>(values.size())};
        const auto *raw = reinterpret_cast<const unsigned char *>(header);
        bytes.insert(bytes.end(), raw, raw + sizeof header);
        const auto *data = reinterpret_cast<const unsigned char *>(values.data());
        bytes.insert(bytes.end(), data, data + values.size() * sizeof(T));
        while (bytes.size() % 8 != 0) bytes.push_back(0);
        return *this;
    }
};

constexpr uint32_t kI32 = 1, kI64 = 2, kF32 = 3;

std::vector<unsigned char> model_back(bool flash, bool autofocus) {
    FactsList list;
    list.add<int32_t>(1, kI32, {1})                                  // FACING: back
            .add<int32_t>(2, kI32, {90})                             // ORIENTATION
            .add<int32_t>(3, kI32, {4032, 3024})                     // PIXEL_ARRAY
            .add<int32_t>(4, kI32, {0, 0, 4032, 3024})               // ACTIVE_ARRAY
            .add<float>(5, kF32, {5.6f, 4.2f})                       // PHYSICAL_SIZE
            .add<float>(6, kF32, {4.3f})                             // FOCAL_LENGTHS
            .add<float>(7, kF32, {1.8f})                             // APERTURES
            .add<float>(8, kF32, {autofocus ? 10.0f : 0.0f})         // MIN_FOCUS_DISTANCE
            .add<int32_t>(9, kI32, {flash ? 1 : 0})                  // FLASH
            .add<int32_t>(10, kI32, {-12, 12, 1, 6})                 // AE_COMPENSATION
            .add<float>(11, kF32, {1.0f, 8.0f})                      // ZOOM
            .add<int32_t>(12, kI32, {15, 30, 30, 30})                // FPS_RANGES
            .add<int32_t>(13, kI32, {100, 1600})                     // ISO
            .add<int64_t>(14, kI64, {100000, 66666666})              // EXPOSURE_NS
            .add<int32_t>(15, kI32, autofocus ? std::vector<int32_t>{0, 1, 4} : std::vector<int32_t>{0})
            .add<int32_t>(16, kI32, {1})                             // AWB_MODES
            .add<int32_t>(17, kI32, {0})                             // FACES
            .add<int64_t>(18, kI64, {0x23, 640, 480, 33333333, 0x23, 1280, 720, 33333333, 0x23, 1920, 1080,
                                     33333333, 0x100, 4032, 3024, 33333333})  // STREAMS
            .add<int64_t>(19, kI64, {0x100, 4032, 3024, 200000000});  // STALLS
    return list.bytes;
}

TEST(the_hosts_facts_make_a_limited_camera) {
    const auto facts = model_back(true, true);
    camera::Facts got;
    CHECK(camera::read_facts(facts.data(), facts.size(), &got));
    CHECK(got.facing == 1 && got.orientation == 90);
    const camera::StaticInfo &info = got.info;
    CHECK(info.hardware_level == 0);  // LIMITED
    CHECK(info.active_array == (std::array<int32_t, 4>{0, 0, 4032, 3024}));
    CHECK(info.focus_calibration && *info.focus_calibration == 0);  // UNCALIBRATED: the host's lens position
    CHECK(info.ae_modes && *info.ae_modes == (std::vector<int32_t>{1, 2, 3}));  // ON and the flash's
    CHECK(info.af_modes && *info.af_modes == (std::vector<int32_t>{0, 1, 4}));
    CHECK(info.max_regions == (std::array<int32_t, 3>{1, 0, 1}));
    CHECK(info.ae_compensation_range == (std::array<int32_t, 2>{-12, 12}));
    CHECK(info.ae_compensation_step == (std::array<int32_t, 2>{1, 6}));
    CHECK(info.max_digital_zoom == 8.0f);
    // Each YUV size is a PRIVATE and a YUV_420_888 stream, the JPEG a BLOB, each with its duration.
    std::set<std::tuple<int32_t, int32_t, int32_t>> streams;
    for (const auto &stream : info.streams) streams.insert({stream.format, stream.width, stream.height});
    CHECK(streams.count({0x22, 1920, 1080}) == 1 && streams.count({0x23, 1920, 1080}) == 1);
    CHECK(streams.count({0x21, 4032, 3024}) == 1 && streams.size() == 7);
    CHECK(info.min_durations.size() == 7);
    const auto params = entries(camera::legacy_parameters(info, camera::kVideoBounds));
    CHECK(params.at("preview-size") == "1920x1080" && params.at("picture-size") == "4032x3024");
    CHECK(params.at("preview-size-values") == "640x480,1280x720,1920x1080");
    CHECK(params.at("focus-mode-values") == "auto,continuous-picture");
    CHECK(params.at("flash-mode-values") == "off,auto,on,torch");
    CHECK(params.at("preview-fps-range") == "15000,30000" && params.at("max-zoom") == "99");
    CHECK(params.count("scene-mode") == 0);  // no scene modes
    CHECK(params.at("max-num-detected-faces-hw") == "0");
}

TEST(a_front_camera_without_flash_or_focus) {
    const auto facts = model_back(false, false);
    camera::Facts got;
    CHECK(camera::read_facts(facts.data(), facts.size(), &got));
    CHECK(got.info.ae_modes && *got.info.ae_modes == (std::vector<int32_t>{1}));
    CHECK(got.info.max_regions == (std::array<int32_t, 3>{1, 0, 0}));
    const auto params = entries(camera::legacy_parameters(got.info, camera::kVideoBounds));
    CHECK(params.at("focus-mode") == "fixed" && params.count("flash-mode") == 0);
}

TEST(facts_that_do_not_hold_are_refused) {
    camera::Facts got;
    std::vector<unsigned char> facts = model_back(true, true);
    facts.resize(facts.size() - 8);
    CHECK(!camera::read_facts(facts.data(), facts.size(), &got));
    const auto nothing = FactsList().add<int32_t>(1, kI32, {1}).bytes;  // no streams, no array
    CHECK(!camera::read_facts(nothing.data(), nothing.size(), &got));
}

/// The parameters with `key` set to `value`, as Camera.Parameters.set and flatten give them: the key set last moves
/// to the end, so that the frame rate's two keys keep their set order.
std::string with(const std::string &flattened, const std::string &key, const std::string &value) {
    std::string out;
    for (const auto &[k, v] : entries(flattened)) {
        if (k != key) out += (out.empty() ? "" : ";") + k + "=" + v;
    }
    return out + ";" + key + "=" + value;
}

TEST(set_parameters_takes_what_the_lists_hold_and_refuses_the_rest) {
    const std::string initial = camera::legacy_parameters(emulator(), emulator_video);
    CHECK(camera::accept_parameters(initial, initial));
    CHECK(camera::accept_parameters(initial, with(initial, "preview-size", "160x120")));
    CHECK(camera::accept_parameters(initial, with(initial, "picture-size", "640x480")));
    CHECK(camera::accept_parameters(initial, with(initial, "focus-mode", "continuous-picture")));
    CHECK(camera::accept_parameters(initial, with(initial, "zoom", "99")));
    CHECK(camera::accept_parameters(initial, with(initial, "preview-fps-range", "15000,30000")));
    CHECK(camera::accept_parameters(initial, with(initial, "flash-mode", "torch")));
    CHECK(camera::accept_parameters(initial, with(initial, "rotation", "270")));
    const auto refused = [&](const char *key, const char *value) {
        return !camera::accept_parameters(initial, with(initial, key, value));
    };
    CHECK(refused("preview-size", "1x1"));
    CHECK(refused("picture-size", "100x100"));
    CHECK(refused("preview-fps-range", "1000,2000"));
    CHECK(refused("preview-frame-rate", "60"));  // set after the range: the single rate counts, in no range
    CHECK(camera::accept_parameters(initial, with(initial, "preview-frame-rate", "15")));
    CHECK(refused("preview-format", "rgb565"));
    CHECK(refused("jpeg-quality", "101"));
    CHECK(refused("jpeg-quality", "0"));
    CHECK(refused("jpeg-thumbnail-quality", "-1"));
    CHECK(refused("rotation", "45"));
    CHECK(refused("effect", "sepia"));
    CHECK(refused("antibanding", "60hz"));
    CHECK(refused("flash-mode", "red-eye"));
    CHECK(refused("whitebalance", "twilight"));
    CHECK(refused("focus-mode", "edof"));
    CHECK(refused("exposure-compensation", "10"));
    CHECK(refused("zoom", "100"));
    CHECK(refused("video-size", "1856x1392"));
    // Without a frame rate of either kind, as Parameters::set refuses it.
    auto map = entries(initial);
    map.erase("preview-fps-range");
    map.erase("preview-frame-rate");
    std::string none;
    for (const auto &[k, v] : map) none += (none.empty() ? "" : ";") + k + "=" + v;
    CHECK(!camera::accept_parameters(initial, none));
}

}  // namespace

int main() { return apk::testing::run_all(); }
