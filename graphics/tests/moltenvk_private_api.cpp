// ADR-0044 b), test 4's first part: our MoltenVK, built with MVK_USE_METAL_PRIVATE_API=1 and
// MVK_CONFIG_USE_METAL_PRIVATE_API=0, keeps Apple's private Metal API off by default and turns it on for one
// VkInstance through VK_EXT_layer_settings, as graphics/host will for ANGLE's instance (decision 2). A host
// program without guest code opens build/moltenvk/macos/libMoltenVK.dylib with dlopen and reads
// VkPhysicalDeviceProvokingVertexFeaturesEXT::provokingVertexLast, which MoltenVK 1.4.2 answers from the
// instance's setting alone (MVKDevice.mm): 0 without the setting, 1 with it, and both at once from two
// instances that live side by side, as ANGLE's and guest Vulkan's do in one app. MoltenVK as released has no
// private API, so it answers 0 with the setting too.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "moltenvk-private-api: %s\n", what.c_str());
    std::exit(1);
}

void check(VkResult result, const char *what) {
    if (result != VK_SUCCESS) {
        fail(std::string(what) + " failed: " + std::to_string(result));
    }
}

PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;

template <typename Function>
Function instance_function(VkInstance instance, const char *name) {
    auto function = reinterpret_cast<Function>(get_instance_proc(instance, name));
    if (function == nullptr) {
        fail(std::string("MoltenVK has no ") + name);
    }
    return function;
}

#define VKI(instance, name) instance_function<PFN_vk##name>(instance, "vk" #name)

/// An instance, with MoltenVK's private API turned on through VK_EXT_layer_settings when `private_api` is set.
VkInstance make_instance(bool private_api) {
    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "moltenvk-private-api";
    application.apiVersion = VK_API_VERSION_1_3;
    const VkBool32 on = VK_TRUE;
    VkLayerSettingEXT setting = {};
    setting.pLayerName = "MoltenVK";
    setting.pSettingName = "MVK_CONFIG_USE_METAL_PRIVATE_API";
    setting.type = VK_LAYER_SETTING_TYPE_BOOL32_EXT;
    setting.valueCount = 1;
    setting.pValues = &on;
    VkLayerSettingsCreateInfoEXT settings = {};
    settings.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    settings.settingCount = 1;
    settings.pSettings = &setting;
    const char *extension = VK_EXT_LAYER_SETTINGS_EXTENSION_NAME;
    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    if (private_api) {
        info.pNext = &settings;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &extension;
    }
    VkInstance instance = VK_NULL_HANDLE;
    check(VKI(VK_NULL_HANDLE, CreateInstance)(&info, nullptr, &instance), "vkCreateInstance");
    return instance;
}

/// provokingVertexLast of the instance's first physical device.
VkBool32 provoking_vertex_last(VkInstance instance) {
    uint32_t count = 0;
    VKI(instance, EnumeratePhysicalDevices)(instance, &count, nullptr);
    if (count == 0) {
        fail("no physical device");
    }
    std::vector<VkPhysicalDevice> physical_devices(count);
    check(VKI(instance, EnumeratePhysicalDevices)(instance, &count, physical_devices.data()),
          "vkEnumeratePhysicalDevices");
    VkPhysicalDeviceProvokingVertexFeaturesEXT provoking = {};
    provoking.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features = {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &provoking;
    VKI(instance, GetPhysicalDeviceFeatures2)(physical_devices[0], &features);
    return provoking.provokingVertexLast;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: moltenvk-private-api <libMoltenVK.dylib>");
    }
    if (access(argv[1], F_OK) != 0) {
        std::printf("skipped: %s is not built, see tools/build-moltenvk.sh\n", argv[1]);
        return 0;
    }
    // MoltenVK reads its settings from the environment too; only the instance's own setting may count here.
    unsetenv("MVK_CONFIG_USE_METAL_PRIVATE_API");
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        fail(std::string("dlopen: ") + dlerror());
    }
    get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
    if (get_instance_proc == nullptr) {
        fail("libMoltenVK does not export vkGetInstanceProcAddr");
    }

    int failures = 0;
    const auto expect = [&failures](const char *what, VkBool32 actual, VkBool32 wanted) {
        std::printf("%s: provokingVertexLast %u%s\n", what, actual, actual == wanted ? "" : " (FAIL)");
        if (actual != wanted) {
            ++failures;
        }
    };
    VkInstance without = make_instance(false);
    expect("without the setting", provoking_vertex_last(without), VK_FALSE);
    VKI(without, DestroyInstance)(without, nullptr);
    VkInstance with = make_instance(true);
    expect("with MVK_CONFIG_USE_METAL_PRIVATE_API", provoking_vertex_last(with), VK_TRUE);
    VKI(with, DestroyInstance)(with, nullptr);

    // Both at once, made in either order: the setting stays with its instance.
    VkInstance first = make_instance(true);
    VkInstance second = make_instance(false);
    VkInstance third = make_instance(true);
    expect("side by side, the first with the setting", provoking_vertex_last(first), VK_TRUE);
    expect("side by side, the second without it", provoking_vertex_last(second), VK_FALSE);
    expect("side by side, the third with it", provoking_vertex_last(third), VK_TRUE);
    VKI(first, DestroyInstance)(first, nullptr);
    VKI(second, DestroyInstance)(second, nullptr);
    VKI(third, DestroyInstance)(third, nullptr);

    std::printf(failures == 0 ? "moltenvk-private-api: ok\n" : "moltenvk-private-api: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
