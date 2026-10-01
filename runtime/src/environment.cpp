#include "environment.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace apkrt::environment {
namespace {

constexpr std::string_view boot_class_path =
    "/apex/com.android.art/javalib/core-oj.jar:/apex/com.android.art/javalib/core-libart.jar:"
    "/apex/com.android.art/javalib/core-icu4j.jar";

/// An app's environment on Android 15, in its order (the emulator's, 2026-10-01): init's PATH, then
/// init.environ.rc (system/core/rootdir/init.environ.rc.in), then the class paths that init loads after
/// derive_classpath. BOOTCLASSPATH names our jars; DEX2OATBOOTCLASSPATH is the part of it before the
/// mainline framework's jars, of which we have none: all of it. Zygote's socket numbers stay out.
constexpr std::pair<std::string_view, std::string_view> init_environment[] = {
    {"PATH",
     "/product/bin:/apex/com.android.runtime/bin:/apex/com.android.art/bin:/system_ext/bin:/system/bin:/system/xbin:"
     "/odm/bin:/vendor/bin:/vendor/xbin"},
    {"ANDROID_BOOTLOGO", "1"},
    {"ANDROID_ROOT", "/system"},
    {"ANDROID_ASSETS", "/system/app"},
    {"ANDROID_DATA", "/data"},
    {"ANDROID_STORAGE", "/storage"},
    {"ANDROID_ART_ROOT", "/apex/com.android.art"},
    {"ANDROID_I18N_ROOT", "/apex/com.android.i18n"},
    {"ANDROID_TZDATA_ROOT", "/apex/com.android.tzdata"},
    {"EXTERNAL_STORAGE", "/sdcard"},
    {"ASEC_MOUNTPOINT", "/mnt/asec"},
    {"DOWNLOAD_CACHE", "/data/cache"},
    {"BOOTCLASSPATH", boot_class_path},
    {"DEX2OATBOOTCLASSPATH", boot_class_path},
    {"SYSTEMSERVERCLASSPATH",
     "/system/framework/com.android.location.provider.jar:/system/framework/services.jar:"
     "/apex/com.android.adservices/javalib/service-adservices.jar:"
     "/apex/com.android.adservices/javalib/service-sdksandbox.jar:"
     "/apex/com.android.appsearch/javalib/service-appsearch.jar:/apex/com.android.art/javalib/service-art.jar:"
     "/apex/com.android.configinfrastructure/javalib/service-configinfrastructure.jar:"
     "/apex/com.android.healthfitness/javalib/service-healthfitness.jar:"
     "/apex/com.android.media/javalib/service-media-s.jar:"
     "/apex/com.android.ondevicepersonalization/javalib/service-ondevicepersonalization.jar:"
     "/apex/com.android.permission/javalib/service-permission.jar:/apex/com.android.rkpd/javalib/service-rkp.jar:"
     "/apex/com.android.virt/javalib/service-virtualization.jar"},
    {"STANDALONE_SYSTEMSERVER_JARS",
     "/apex/com.android.btservices/javalib/service-bluetooth.jar:"
     "/apex/com.android.devicelock/javalib/service-devicelock.jar:"
     "/apex/com.android.os.statsd/javalib/service-statsd.jar:/apex/com.android.profiling/javalib/service-profiling.jar:"
     "/apex/com.android.scheduling/javalib/service-scheduling.jar:"
     "/apex/com.android.tethering/javalib/service-connectivity.jar:/apex/com.android.uwb/javalib/service-uwb.jar:"
     "/apex/com.android.wifi/javalib/service-wifi.jar"},
};

std::string_view name_of(std::string_view entry) {
    return entry.substr(0, entry.find('='));
}

}  // namespace

std::vector<std::string> at_start(const std::vector<std::string> &caller) {
    std::vector<std::string> result;
    for (const auto &[name, value] : init_environment) {
        const auto given = std::find_if(caller.begin(), caller.end(),
                                        [&](const std::string &entry) { return name_of(entry) == name; });
        result.push_back(given != caller.end() ? *given : std::string(name) + "=" + std::string(value));
    }
    for (const auto &entry : caller) {
        const bool of_init = std::any_of(std::begin(init_environment), std::end(init_environment),
                                         [&](const auto &pair) { return pair.first == name_of(entry); });
        if (!of_init) result.push_back(entry);
    }
    return result;
}

}  // namespace apkrt::environment
