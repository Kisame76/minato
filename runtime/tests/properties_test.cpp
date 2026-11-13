// Checks the device and version values of the property area (ADR-0039 decision 9) as android.os.Build reads them at
// its start: the ABI lists, whose 64-bit one gives Build.CPU_ABI; Android 15's version as a user build, whose code
// names give Build.VERSION.ACTIVE_CODENAMES, the list of known ones too long for a short value; the device as the
// host names it, not an emulator, and no fingerprint, so that Build derives one; the density and the GLES version;
// the Vulkan driver's name only while graphics/host provides Vulkan.
// The area is read as bionic reads it (prop_area.cpp's find_property, prop_info's short and long values);
// fixture.property-test reads it through bionic itself.
#include "install_sources.h"
#include "properties.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t header_size = 128;
// struct prop_trie_node: namelen, prop, left, right, children, then the name.
constexpr std::uint32_t node_prop = 4, node_left = 8, node_right = 12, node_children = 16, node_name = 20;
// struct prop_info: serial, then the value, or for a long one a message for get and the value's offset.
constexpr std::uint32_t info_value = 4, info_long_offset = info_value + 56, info_size = 96;
constexpr std::uint32_t long_flag = 1u << 16;
constexpr std::string_view long_error = "Must use __system_property_read_callback() to read";

std::string area = apkrt::properties::area();  // without providers; the Vulkan test builds it again

std::uint32_t load(std::uint32_t offset) {
    std::uint32_t value;
    std::memcpy(&value, area.data() + header_size + offset, sizeof value);
    return value;
}

/// The offset of `name`'s prop_info, as bionic's find_property follows the trie; nothing when it is not there.
std::optional<std::uint32_t> find(std::string_view name) {
    std::uint32_t current = 0;  // the root node
    for (std::string_view rest = name;;) {
        const auto dot = rest.find('.');
        const auto part = rest.substr(0, dot);
        std::uint32_t node = load(current + node_children);
        while (node != 0) {
            const std::uint32_t length = load(node);
            const std::string_view node_part(area.data() + header_size + node + node_name, length);
            const int order = part.size() != length ? (part.size() < length ? -1 : 1) : part.compare(node_part);
            if (order == 0) break;
            node = load(node + (order < 0 ? node_left : node_right));
        }
        if (node == 0) return std::nullopt;
        current = node;
        if (dot == std::string_view::npos) break;
        rest = rest.substr(dot + 1);
    }
    const std::uint32_t info = load(current + node_prop);
    return info != 0 ? std::optional(info) : std::nullopt;
}

/// `name`'s value as __system_property_read_callback gives it, a long one whole; nothing when it is not there.
std::optional<std::string> get(std::string_view name) {
    const auto info = find(name);
    if (!info) return std::nullopt;
    const std::uint32_t serial = load(*info);
    const char *const at = area.data() + header_size + *info;
    if ((serial & long_flag) != 0) return std::string(at + load(*info + info_long_offset));
    return std::string(at + info_value, serial >> 24);
}

/// The host's hw.machine, as iOS names the iPhone ("iPhone16,2") and macOS the Mac's processor.
std::string machine() {
    char name[64] = "";
    std::size_t size = sizeof name;
    CHECK(::sysctlbyname("hw.machine", name, &size, nullptr, 0) == 0);
    return name;
}

/// The host's time zone, as the system names it: an IANA ID such as "Europe/Berlin".
std::string host_time_zone() {
    const CFTimeZoneRef zone = CFTimeZoneCopySystem();
    CHECK(zone != nullptr);
    char name[92] = "";
    CHECK(CFStringGetCString(CFTimeZoneGetName(zone), name, sizeof name, kCFStringEncodingUTF8));
    CFRelease(zone);
    return name;
}

TEST(the_abi_lists_name_arm64_v8a_alone) {
    CHECK(get("ro.product.cpu.abilist") == "arm64-v8a");
    CHECK(get("ro.product.cpu.abilist64") == "arm64-v8a");
    CHECK(get("ro.product.cpu.abilist32") == "");
}

TEST(the_version_is_android_15s_as_a_user_build) {
    // [verified: the emulator's getprop, 2026-10-01], but for the build's type and tags.
    CHECK(get("ro.build.id") == "AE3A.240806.019");
    CHECK(get("ro.build.version.incremental") == "12368160");
    CHECK(get("ro.build.version.release_or_preview_display") == "15");
    CHECK(get("ro.build.version.all_codenames") == "REL");
    CHECK(get("ro.build.version.known_codenames") ==
          "Base,Base11,Cupcake,Donut,Eclair,Eclair01,EclairMr1,Froyo,Gingerbread,GingerbreadMr1,Honeycomb,"
          "HoneycombMr1,HoneycombMr2,IceCreamSandwich,IceCreamSandwichMr1,JellyBean,JellyBeanMr1,JellyBeanMr2,"
          "Kitkat,KitkatWatch,Lollipop,LollipopMr1,M,N,NMr1,O,OMr1,P,Q,R,S,Sv2,Tiramisu,UpsideDownCake,"
          "VanillaIceCream");
    CHECK(get("ro.build.version.min_supported_target_sdk") == "28");
    CHECK(get("ro.build.type") == "user");
    CHECK(get("ro.build.tags") == "release-keys");
}

TEST(a_long_value_is_written_as_bionic_writes_it) {
    const auto info = find("ro.build.version.known_codenames");
    CHECK(info.has_value());
    if (!info) return;
    CHECK(load(*info) == (static_cast<std::uint32_t>(long_error.size()) << 24 | long_flag));
    CHECK(std::string_view(area.data() + header_size + *info + info_value) == long_error);
    // The value lies past the prop_info and its name, at an offset from the prop_info.
    CHECK(load(*info + info_long_offset) >= info_size + std::strlen("ro.build.version.known_codenames") + 1);
}

TEST(the_device_is_the_host_and_no_emulator) {
    CHECK(get("ro.product.manufacturer") == "Apple");
    CHECK(get("ro.product.brand") == "Apple");
    CHECK(get("ro.hardware") == "minato");
    CHECK(get("ro.product.model") == machine());
    CHECK(get("ro.product.device") == machine());
    // Build derives its fingerprint from the values above; no emulator's flag.
    CHECK(!get("ro.build.fingerprint"));
    CHECK(!get("ro.boot.qemu"));
    CHECK(!get("ro.kernel.qemu"));
}

TEST(the_density_is_480_and_gles_3_0) {
    CHECK(get("ro.sf.lcd_density") == "480");
    CHECK(get("ro.opengles.version") == "196608");
}

TEST(the_earlier_values_stay) {
    CHECK(get("ro.build.version.sdk") == "35");
    CHECK(get("ro.build.version.release") == "15");
    CHECK(get("ro.debuggable") == "0");
    CHECK(get("dalvik.vm.heapsize") == "512m");
}

TEST(the_time_zone_is_the_hosts) {
    // Android keeps the device's zone in persist.sys.timezone, which bionic's localtime and libcore's default time
    // zone read: the iPhone's zone, or the Mac's.
    CHECK(get("persist.sys.timezone") == host_time_zone());
}

TEST(libcutils_ashmem_takes_memfd_create) {
    // ashmem-dev.cpp's __has_memfd_support wants both before it tries memfd_create; Minato has no /dev/ashmem.
    CHECK(get("ro.treble.enabled") == "true");
    CHECK(get("sys.use_memfd") == "true");
}

TEST(the_vulkan_driver_is_named_only_while_graphics_host_provides_vulkan) {
    // ro.hardware.vulkan names the driver that Android 15's libvulkan loads, vulkan.<value>.so (driver.cpp), and the
    // framework reports the vulkan.* features only with it (ADR-0045). graphics/host provides Vulkan with MoltenVK
    // on the iPhone and the Mac, and not in the Simulator (ADR-0030 decision 3).
    CHECK(!get("ro.hardware.vulkan"));
    const apkrt_host_provider provider = [](void *, void **, std::size_t, std::uint64_t) { return 0; };
    CHECK(apkrt_set_host_provider("gles", provider, nullptr) == 0);
    area = apkrt::properties::area();
    CHECK(!get("ro.hardware.vulkan"));
    CHECK(apkrt_set_host_provider("vulkan", provider, nullptr) == 0);
    area = apkrt::properties::area();
    CHECK(get("ro.hardware.vulkan") == "moltenvk");
    CHECK(apkrt_set_host_provider("vulkan", nullptr, nullptr) == 0);
    area = apkrt::properties::area();
    CHECK(!get("ro.hardware.vulkan"));
    CHECK(apkrt_set_host_provider("gles", nullptr, nullptr) == 0);
}

TEST(the_gles_version_follows_angles_backend) {
    // ADR-0044 d): ES 3.1 while ANGLE draws on its Vulkan backend, which graphics/host sets before the program
    // starts; ES 3.0 on Metal. ActivityManager's reqGlEsVersion reads the property (ApplicationPackageManager).
    CHECK(apkrt_set_opengles_version(0x30001) == 0);
    area = apkrt::properties::area();
    CHECK(get("ro.opengles.version") == "196609");
    CHECK(apkrt_set_opengles_version(0x30002) == -1);
    CHECK(apkrt_set_opengles_version(0x20000) == -1);
    area = apkrt::properties::area();
    CHECK(get("ro.opengles.version") == "196609");
    CHECK(apkrt_set_opengles_version(0x30000) == 0);
    area = apkrt::properties::area();
    CHECK(get("ro.opengles.version") == "196608");
}

TEST(a_device_profile_replaces_the_names_build_reads_and_nothing_else) {
    // ADR-0065: the defaults are the host's (the test above); a profile gives a phone's names for the Build fields,
    // and leaves the build type, the keys, the debuggable flag, the density and the fingerprint as they are.
    CHECK(apkrt_set_device_profile("pixel") == 0);
    area = apkrt::properties::area();
    CHECK(get("ro.product.manufacturer") == "Google");
    CHECK(get("ro.product.brand") == "google");
    CHECK(get("ro.product.model") == "Pixel 9");
    CHECK(get("ro.product.device") == "tokay");
    CHECK(get("ro.product.name") == "tokay");
    CHECK(get("ro.hardware") == "tokay");
    CHECK(get("ro.soc.manufacturer") == "Google");
    CHECK(get("ro.soc.model") == "Tensor G4");
    CHECK(apkrt_set_device_profile("galaxy") == 0);
    area = apkrt::properties::area();
    CHECK(get("ro.product.manufacturer") == "samsung");
    CHECK(get("ro.product.brand") == "samsung");
    CHECK(get("ro.product.model") == "SM-S921B");
    CHECK(get("ro.product.device") == "e1s");
    CHECK(get("ro.product.name") == "e1sxxx");
    CHECK(get("ro.hardware") == "s5e9945");
    CHECK(get("ro.soc.manufacturer") == "Samsung");
    CHECK(get("ro.soc.model") == "S5E9945");
    CHECK(get("ro.build.type") == "user");
    CHECK(get("ro.build.tags") == "release-keys");
    CHECK(get("ro.debuggable") == "0");
    CHECK(get("ro.sf.lcd_density") == "480");
    CHECK(get("ro.product.cpu.abilist") == "arm64-v8a");
    CHECK(!get("ro.build.fingerprint"));
    CHECK(!get("ro.boot.qemu"));
}

TEST(a_profile_names_the_gpu_in_properties_that_exist_only_under_it) {
    // ADR-0065 step 4 and its amendment: the front libs of GLES and Vulkan read these to replace the GPU's identity
    // with the profile's real device's; without a profile none of them exists, so the host's own (ANGLE's,
    // MoltenVK's) stays.
    static const char *const names[] = {
        "ro.minato.gpu.gl_vendor",         "ro.minato.gpu.gl_renderer",      "ro.minato.gpu.gl_version_tag",
        "ro.minato.gpu.vk_device_name",    "ro.minato.gpu.vk_vendor_id",     "ro.minato.gpu.vk_device_id",
        "ro.minato.gpu.vk_driver_version", "ro.minato.gpu.vk_driver_id",     "ro.minato.gpu.vk_driver_name",
        "ro.minato.gpu.vk_driver_info"};
    CHECK(apkrt_set_device_profile("") == 0);
    area = apkrt::properties::area();
    for (const char *name : names) CHECK(!get(name));
    CHECK(apkrt_set_device_profile("pixel") == 0);
    area = apkrt::properties::area();
    for (const char *name : names) CHECK(get(name).has_value());
    CHECK(get("ro.minato.gpu.gl_vendor") == "ARM");
    CHECK(get("ro.minato.gpu.gl_renderer") == "Mali-G715");
    CHECK(get("ro.minato.gpu.vk_device_name") == "Mali-G715");
    CHECK(get("ro.minato.gpu.vk_vendor_id") == "5045");              // 0x13B5
    CHECK(get("ro.minato.gpu.vk_device_id") == "3097624576");        // 0xB8A20000
    CHECK(get("ro.minato.gpu.vk_driver_version") == "209715200");    // r50p0
    CHECK(get("ro.minato.gpu.vk_driver_id") == "9");                 // VK_DRIVER_ID_ARM_PROPRIETARY
    CHECK(apkrt_set_device_profile("galaxy") == 0);
    area = apkrt::properties::area();
    CHECK(get("ro.minato.gpu.gl_vendor") == "Google Inc. (Samsung Electronics Co., Ltd.)");
    // Past PROP_VALUE_MAX: a long read-only value, whole.
    CHECK(get("ro.minato.gpu.gl_renderer") ==
          "ANGLE (Samsung Electronics Co. Ltd., Vulkan 1.3.279 (Samsung Xclipse 940 (0x02600200)), Samsung "
          "Proprietary driver-24.3.9)");
    CHECK(get("ro.minato.gpu.vk_device_name") == "Samsung Xclipse 940");
    CHECK(get("ro.minato.gpu.vk_vendor_id") == "5197");              // 0x144D
    CHECK(get("ro.minato.gpu.vk_device_id") == "39846400");          // 0x02600200
    CHECK(get("ro.minato.gpu.vk_driver_version") == "100675593");    // 24.3.9
    CHECK(get("ro.minato.gpu.vk_driver_id") == "21");                // VK_DRIVER_ID_SAMSUNG_PROPRIETARY
    CHECK(apkrt_set_device_profile("") == 0);
    area = apkrt::properties::area();
    CHECK(!get("ro.minato.gpu.gl_renderer"));
}

TEST(an_unknown_profile_is_refused_and_the_default_comes_back) {
    CHECK(apkrt_set_device_profile("pixel") == 0);
    CHECK(apkrt_set_device_profile("nokia") == -1);
    CHECK(apkrt_set_device_profile("Pixel") == -1);
    area = apkrt::properties::area();
    CHECK(get("ro.product.model") == "Pixel 9");  // the refused name left the profile as it was
    CHECK(apkrt_set_device_profile("") == 0);
    area = apkrt::properties::area();
    CHECK(get("ro.product.manufacturer") == "Apple");
    CHECK(get("ro.product.brand") == "Apple");
    CHECK(get("ro.hardware") == "minato");
    CHECK(get("ro.product.model") == machine());
    CHECK(get("ro.product.device") == machine());
    CHECK(!get("ro.product.name"));
    CHECK(!get("ro.soc.model"));
    CHECK(apkrt_set_device_profile("galaxy") == 0);
    CHECK(apkrt_set_device_profile(nullptr) == 0);
    area = apkrt::properties::area();
    CHECK(get("ro.product.manufacturer") == "Apple");
}

TEST(install_sources_publishes_minato_install_sources_from_the_grant) {
    // canRequestPackageInstalls reads minato.install_sources at the start (the property area is a start snapshot); the
    // host sets the grant with apkrt_set_install_sources before the program starts. Denied by default.
    apkrt::install_sources::reset();
    area = apkrt::properties::area();
    CHECK(get("minato.install_sources") == "0");
    apkrt::install_sources::set(true);
    area = apkrt::properties::area();
    CHECK(get("minato.install_sources") == "1");
    apkrt::install_sources::reset();
    area = apkrt::properties::area();  // leave the default for any later test
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
