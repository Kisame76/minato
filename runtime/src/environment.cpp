#include "environment.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace apkrt::environment {
namespace {

constexpr std::string_view boot_class_path =
    "/apex/com.android.art/javalib/core-oj.jar:/apex/com.android.art/javalib/core-libart.jar:"
    "/apex/com.android.art/javalib/core-icu4j.jar";

/// init.environ.rc of Android 15 (system/core/rootdir/init.environ.rc.in), in its order, then the class
/// paths that init loads after derive_classpath. DEX2OATBOOTCLASSPATH is the part of BOOTCLASSPATH
/// before the mainline framework's jars, of which we have none: all of it.
constexpr std::pair<std::string_view, std::string_view> init_environment[] = {
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
    {"BOOTCLASSPATH", boot_class_path},
    {"DEX2OATBOOTCLASSPATH", boot_class_path},
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
