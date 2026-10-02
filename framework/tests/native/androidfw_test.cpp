// ADR-0039 b3.2's test: libandroidfw, built as guest code with the libraries it links (libbase, libcutils,
// libutils, libziparchive, libincfs-utils), reads framework-res and an app's resources as Android's AssetManager
// does through it, in the guest on our loader.
//   androidfw-test <framework-res folder> <framework-res.apk> <app.apk>
// framework-res comes once as the folder that Minato ships (ApkAssets.loadFromDir's DirectoryAssetsProvider) and
// once as the APK (ApkAssets.load's ZipAssetsProvider), both as the system's (PROPERTY_SYSTEM); the app is
// tools/aapt2/check/res and tools/aapt2/check/assets linked against framework-res. Each part prints
// "androidfw-test: " lines, which framework/tests/androidfw_run.py compares with
// fixtures/expected/androidfw-test.txt, the values of the resources' sources. It ends with 0 when every part could be read, else with 1.
#include <androidfw/ApkAssets.h>
#include <androidfw/AssetDir.h>
#include <androidfw/AssetManager2.h>
#include <androidfw/AssetsProvider.h>
#include <androidfw/ResourceTypes.h>
#include <androidfw/ResourceUtils.h>
#include <androidfw/Util.h>
#include <stdio.h>

#include <string>
#include <vector>

using android::ApkAssets;
using android::AssetManager2;
using android::Res_value;
using android::ResTable_config;

namespace {

int failures = 0;

void print(const std::string &line) {
    printf("androidfw-test: %s\n", line.c_str());
}

void fail(const std::string &what) {
    print("FAILED " + what);
    failures++;
}

std::string hex(uint32_t value) {
    char text[16];
    snprintf(text, sizeof text, "0x%08x", value);
    return text;
}

std::string name_of(const AssetManager2 &assets, uint32_t resid) {
    auto name = assets.GetResourceName(resid);
    return name.has_value() ? android::ToFormattedResourceString(*name) : hex(resid);
}

// A complex value's number, as TypedValue.complexToFloat computes it.
float complex_value(uint32_t data) {
    static const float radix_scale[] = {1.0f / (1 << 8), 1.0f / (1 << 15), 1.0f / (1 << 23), 1.0f / (1u << 31)};
    const int32_t mantissa = static_cast<int32_t>(data & (Res_value::COMPLEX_MANTISSA_MASK << Res_value::COMPLEX_MANTISSA_SHIFT));
    return mantissa * radix_scale[(data >> Res_value::COMPLEX_RADIX_SHIFT) & Res_value::COMPLEX_RADIX_MASK];
}

// A Res_value as text: strings from the cookie's pool, references and attributes by name, dimensions with
// their unit.
std::string text_of(const AssetManager2 &assets, uint8_t type, uint32_t data, android::ApkAssetsCookie cookie) {
    char text[64];
    switch (type) {
    case Res_value::TYPE_STRING: {
        const auto *pool = assets.GetStringPoolForCookie(cookie);
        if (pool == nullptr) return "<no string pool>";
        auto string = pool->string8ObjectAt(data);
        return string.has_value() ? "\"" + std::string(string->c_str()) + "\"" : "<no string>";
    }
    case Res_value::TYPE_REFERENCE:
        return "@" + name_of(assets, data);
    case Res_value::TYPE_ATTRIBUTE:
        return "?" + name_of(assets, data);
    case Res_value::TYPE_DIMENSION: {
        static const char *const units[] = {"px", "dp", "sp", "pt", "in", "mm"};
        const uint32_t unit = (data >> Res_value::COMPLEX_UNIT_SHIFT) & Res_value::COMPLEX_UNIT_MASK;
        snprintf(text, sizeof text, "%g%s", complex_value(data), unit < 6 ? units[unit] : "?");
        return text;
    }
    case Res_value::TYPE_INT_DEC:
        snprintf(text, sizeof text, "%d", static_cast<int32_t>(data));
        return text;
    case Res_value::TYPE_INT_HEX:
        snprintf(text, sizeof text, "0x%x", data);
        return text;
    case Res_value::TYPE_INT_BOOLEAN:
        return data != 0 ? "true" : "false";
    case Res_value::TYPE_INT_COLOR_ARGB8:
        snprintf(text, sizeof text, "#%08x", data);
        return text;
    default:
        snprintf(text, sizeof text, "<type 0x%02x> 0x%08x", type, data);
        return text;
    }
}

ResTable_config locale(const char *bcp47) {
    ResTable_config config{};
    config.size = sizeof config;
    if (bcp47 != nullptr) config.setBcp47Locale(bcp47);
    return config;
}

// A value by name under a locale (nullptr for the default configuration).
void value(AssetManager2 &assets, const std::string &name, const char *bcp47) {
    assets.SetConfigurations({locale(bcp47)});
    auto resid = assets.GetResourceId(name);
    if (!resid.has_value()) return fail("no id for " + name);
    auto selected = assets.GetResource(*resid);
    if (!selected.has_value()) return fail("no value for " + name);
    print(name_of(assets, *resid) + " (" + (bcp47 != nullptr ? bcp47 : "") + ") " +
          text_of(assets, selected->type, selected->data, selected->cookie));
}

// A bag (style or plurals) by name with its parents' entries, in the bag's order.
void bag(AssetManager2 &assets, const std::string &name) {
    assets.SetConfigurations({locale(nullptr)});
    auto resid = assets.GetResourceId(name);
    if (!resid.has_value()) return fail("no id for " + name);
    auto resolved = assets.GetBag(*resid);
    if (!resolved.has_value()) return fail("no bag for " + name);
    print(name_of(assets, *resid) + ": " + std::to_string((*resolved)->entry_count) + " entries");
    for (uint32_t i = 0; i < (*resolved)->entry_count; i++) {
        const auto &entry = (*resolved)->entries[i];
        std::string key;
        switch (entry.key) {
        case android::ResTable_map::ATTR_OTHER: key = "other"; break;
        case android::ResTable_map::ATTR_ONE: key = "one"; break;
        default: key = name_of(assets, entry.key);
        }
        print("  " + key + " " + text_of(assets, entry.value.dataType, entry.value.data, entry.cookie));
    }
}

// A file resource (res/...) by name, opened from the APK that holds it.
std::string file_of(AssetManager2 &assets, const std::string &name, android::ApkAssetsCookie *cookie) {
    assets.SetConfigurations({locale(nullptr)});
    auto resid = assets.GetResourceId(name);
    if (!resid.has_value()) return "";
    auto selected = assets.GetResource(*resid);
    if (!selected.has_value() || selected->type != Res_value::TYPE_STRING) return "";
    *cookie = selected->cookie;
    auto path = assets.GetStringPoolForCookie(selected->cookie)->string8ObjectAt(selected->data);
    return path.has_value() ? std::string(path->c_str()) : "";
}

void raw_file(AssetManager2 &assets, const std::string &name) {
    android::ApkAssetsCookie cookie = android::kInvalidCookie;
    const std::string path = file_of(assets, name, &cookie);
    auto asset = path.empty() ? nullptr : assets.OpenNonAsset(path, cookie, android::Asset::ACCESS_BUFFER);
    if (asset == nullptr) return fail("no file for " + name);
    std::string bytes(static_cast<const char *>(asset->getBuffer(false)), asset->getLength());
    while (!bytes.empty() && bytes.back() == '\n') bytes.pop_back();
    print(name + " " + path + " \"" + bytes + "\"");
}

void xml_file(AssetManager2 &assets, const std::string &name) {
    android::ApkAssetsCookie cookie = android::kInvalidCookie;
    const std::string path = file_of(assets, name, &cookie);
    auto asset = path.empty() ? nullptr : assets.OpenNonAsset(path, cookie, android::Asset::ACCESS_BUFFER);
    if (asset == nullptr) return fail("no file for " + name);
    android::ResXMLTree tree;
    if (tree.setTo(asset->getBuffer(true), asset->getLength(), true) != android::NO_ERROR) {
        return fail("no XML tree in " + path);
    }
    std::string elements;
    for (auto event = tree.next(); event != android::ResXMLParser::END_DOCUMENT && event != android::ResXMLParser::BAD_DOCUMENT;
         event = tree.next()) {
        if (event != android::ResXMLParser::START_TAG) continue;
        size_t length = 0;
        const char16_t *element = tree.getElementName(&length);
        elements += " " + android::util::Utf16ToUtf8(std::u16string_view(element, length));
    }
    print(name + " " + path + " elements:" + elements);
}

void asset(AssetManager2 &assets, const std::string &file) {
    auto opened = assets.Open(file, android::Asset::ACCESS_BUFFER);
    if (opened == nullptr) return fail("no asset " + file);
    std::string bytes(static_cast<const char *>(opened->getBuffer(false)), opened->getLength());
    print("asset " + file + " " + std::to_string(bytes.size()) + " bytes, \"" + bytes.substr(0, bytes.find('\n')) +
          "\"");
}

// An asset folder's listing as AssetManager.list gets it, folders with a "/": one level, in AssetDir's order. A
// folder no provider holds lists empty, as a missing prefix does in an APK; null would make list() throw.
void listing(AssetManager2 &assets, const std::string &folder) {
    auto dir = assets.OpenDir(folder);
    if (dir == nullptr) return fail("no listing for asset folder \"" + folder + "\"");
    std::string names;
    for (size_t i = 0; i < dir->getFileCount(); i++) {
        names += " " + std::string(dir->getFileName(i).c_str()) +
                 (dir->getFileType(i) == android::kFileTypeDirectory ? "/" : "");
    }
    print("asset folder \"" + folder + "\" " + std::to_string(dir->getFileCount()) + " entries:" + names);
}

void framework(const char *kind, const android::ApkAssetsPtr &framework_assets) {
    if (framework_assets == nullptr) return fail(std::string("framework-res ") + kind + " did not load");
    const auto &packages = framework_assets->GetLoadedArsc()->GetPackages();
    std::string chunks;
    for (const auto &package : packages) {
        chunks += (chunks.empty() ? " " : ", ") + package->GetPackageName() + " " +
                  hex(package->GetPackageId()).substr(8) + (package->IsSystem() ? " system" : "");
    }
    print(std::string("framework-res ") + kind + ": " + std::to_string(packages.size()) + " package chunks:" + chunks);
    AssetManager2 assets;
    assets.SetApkAssets({framework_assets});
    value(assets, "android:string/cancel", nullptr);
    value(assets, "android:string/cancel", "de");
    bag(assets, "android:style/TextAppearance.Medium");
    asset(assets, "geoid_map/README.md");
    listing(assets, "");
    listing(assets, "geoid_map");
    listing(assets, "no-such-folder");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: androidfw-test <framework-res folder> <framework-res.apk> <app.apk>\n");
        return 2;
    }
    auto folder = ApkAssets::Load(android::DirectoryAssetsProvider::Create(argv[1]), android::PROPERTY_SYSTEM);
    framework("folder", folder);
    auto zip = ApkAssets::Load(argv[2], android::PROPERTY_SYSTEM);
    framework("apk", zip);

    auto app = ApkAssets::Load(argv[3]);
    if (folder == nullptr || app == nullptr) {
        fail("the app's assets did not load");
        return 1;
    }
    AssetManager2 assets;
    assets.SetApkAssets({folder, app});
    value(assets, "io.github.kisame76.minato.check.res:string/app_name", nullptr);
    value(assets, "io.github.kisame76.minato.check.res:string/app_name", "de");
    value(assets, "io.github.kisame76.minato.check.res:string/greeting", "de");
    value(assets, "io.github.kisame76.minato.check.res:color/harbor", nullptr);
    value(assets, "io.github.kisame76.minato.check.res:integer/answer", nullptr);
    value(assets, "io.github.kisame76.minato.check.res:dimen/gap", nullptr);
    bag(assets, "io.github.kisame76.minato.check.res:plurals/apps");
    raw_file(assets, "io.github.kisame76.minato.check.res:raw/data");
    xml_file(assets, "io.github.kisame76.minato.check.res:layout/main");
    value(assets, "android:string/cancel", "de");
    // The app's own asset folder, which the framework-res folder does not hold.
    listing(assets, "notes");
    asset(assets, "notes/hello.txt");
    return failures == 0 ? 0 : 1;
}
