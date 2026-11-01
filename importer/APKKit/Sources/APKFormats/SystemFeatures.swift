import Foundation

/// The system features Minato's PackageManager reports (ADR-0045 decision 4), by name, for bundletool's device spec
/// (ADR-0049 decision 2's follow-up). framework's SystemFeatures.java is their source; SystemFeaturesTests holds this
/// copy to it.
public enum SystemFeatures {
    public static let names: Set<String> = [
        "android.hardware.audio.output", "android.hardware.faketouch", "android.hardware.ram.normal",
        "android.hardware.screen.landscape", "android.hardware.screen.portrait", "android.hardware.touchscreen",
        "android.hardware.touchscreen.multitouch", "android.hardware.touchscreen.multitouch.distinct",
        "android.hardware.touchscreen.multitouch.jazzhand", "android.hardware.vulkan.compute",
        "android.hardware.vulkan.level", "android.hardware.vulkan.version", "android.hardware.wifi",
        "android.software.app_enumeration", "android.software.webview",
    ]
    /// Those that need the session's Vulkan driver, MoltenVK.
    public static let vulkan: Set<String> = ["android.hardware.vulkan.compute", "android.hardware.vulkan.level",
                                             "android.hardware.vulkan.version"]

    /// bundletool's device features: the OpenGL ES version first, as `pm list features` lists it, 3.1 on ANGLE's
    /// Vulkan backend (ADR-0044 d)) and 3.0 on Metal without Vulkan, then each feature by name, Vulkan's only with
    /// Vulkan. A name goes without the version `pm list features`, and so `get-device-spec`, appends
    /// ("android.hardware.vulkan.version=4206592"): bundletool compares a module's feature names with the spec's
    /// strings as they are (`DeviceFeatureMatcher.java`, 1.18.3), so such a module would never install, where by name
    /// it installs exactly when hasSystemFeature(name) holds (the lead's decision).
    static func deviceSpec(vulkan: Bool) -> [String] {
        [vulkan ? "reqGlEsVersion=0x30001" : "reqGlEsVersion=0x30000"]
            + names.subtracting(vulkan ? [] : Self.vulkan).sorted()
    }
}
