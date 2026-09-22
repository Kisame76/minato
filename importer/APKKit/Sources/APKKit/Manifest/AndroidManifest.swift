import Foundation

/// The parts of AndroidManifest.xml that import and compatibility checks need.
/// Resource references (label, icon, versionName) stay unresolved here; `APKAnalyzer` resolves them.
public struct AndroidManifest: Sendable, Equatable {
    public var packageName: String
    public var versionCode: Int64
    public var versionName: ResourceValue?
    public var minSdk: Int
    public var targetSdk: Int

    /// Set when this APK is itself a split (`<manifest split="config.arm64_v8a">`).
    public var splitName: String?
    public var isFeatureSplit: Bool
    /// The base APK cannot run without its splits.
    public var requiresSplits: Bool

    public var label: ResourceValue?
    public var icon: ResourceValue?
    public var roundIcon: ResourceValue?
    /// false for pure native apps (`android:hasCode="false"`).
    public var hasCode: Bool
    /// nil when the attribute is absent (Android then defaults to extracting).
    public var extractNativeLibs: Bool?
    public var isDebuggable: Bool

    /// Fully qualified class started from the launcher (the target of an activity-alias).
    public var launcherActivity: String?
    public var usesNativeActivity: Bool
    /// Library that NativeActivity loads (`android.app.lib_name`, default "main").
    public var nativeLibraryName: String?

    public var requiredGLESVersion: UInt32?
    public var requiredVulkanVersion: UInt32?
    public var requiredVulkanLevel: Int?
    public var requiresAndroidExtensionPack: Bool
    public var requiredFeatures: [String]

    public var permissions: [String]
    public var metaData: [String: ResourceValue]
    public var usesLibraries: [String]
    /// Qualified names of activities, aliases, services, receivers and providers.
    public var components: [String]

    public static let developmentSdk = 10_000

    public init(xml root: XMLElement) throws {
        guard root.name == "manifest" else { throw APKError.invalidManifest("root element is <\(root.name)>") }
        guard let package = root.attribute(named: "package")?.string, !package.isEmpty else {
            throw APKError.invalidManifest("missing package name")
        }
        packageName = package

        let code = Int64(UInt32(bitPattern: Int32(truncatingIfNeeded: root.android(.versionCode)?.integer ?? 0)))
        let major = Int64(root.android(.versionCodeMajor)?.integer ?? 0)
        versionCode = major << 32 | code
        versionName = root.android(.versionName)?.value

        let sdk = root.children(named: "uses-sdk").first
        minSdk = sdk.flatMap { Self.sdkLevel($0.android(.minSdkVersion)) } ?? 1
        targetSdk = sdk.flatMap { Self.sdkLevel($0.android(.targetSdkVersion)) } ?? minSdk

        splitName = root.attribute(named: "split")?.string
        isFeatureSplit = root.android(.isFeatureSplit)?.boolean ?? false

        let application = root.children(named: "application").first
        label = application?.android(.label)?.value
        icon = application?.android(.icon)?.value
        roundIcon = application?.android(.roundIcon)?.value
        hasCode = application?.android(.hasCode)?.boolean ?? true
        extractNativeLibs = application?.android(.extractNativeLibs)?.boolean
        isDebuggable = application?.android(.debuggable)?.boolean ?? false

        var metaData: [String: ResourceValue] = [:]
        var usesLibraries: [String] = []
        var components: [String] = []
        for child in application?.children ?? [] {
            switch child.name {
            case "meta-data":
                if let name = child.android(.name)?.string,
                   let value = child.android(.value)?.value ?? child.android(.resource)?.value {
                    metaData[name] = value
                }
            case "uses-library":
                if let name = child.android(.name)?.string { usesLibraries.append(name) }
            case "activity", "activity-alias", "service", "receiver", "provider":
                if let name = child.android(.name)?.string { components.append(Self.qualify(name, in: package)) }
            default:
                break
            }
        }
        self.metaData = metaData
        self.usesLibraries = usesLibraries
        self.components = components

        let splitTypes = root.android(.requiredSplitTypes)?.string ?? ""
        let splitsFlag = application?.android(.isSplitRequired)?.boolean ?? false
        let splitsMetaData: Bool = switch metaData["com.android.vending.splits.required"] {
        case .boolean(let flag): flag
        case .string(let text): text.lowercased() == "true"
        default: false
        }
        requiresSplits = splitsFlag || splitsMetaData || !splitTypes.isEmpty

        let launcher = application.flatMap { Self.launcher(in: $0, package: package) }
        launcherActivity = launcher?.className
        usesNativeActivity = launcher?.className == "android.app.NativeActivity"
        if usesNativeActivity {
            let libraryName = launcher?.element.children(named: "meta-data")
                .first { $0.android(.name)?.string == "android.app.lib_name" }?
                .android(.value)?.string
            nativeLibraryName = libraryName ?? "main"
        } else {
            nativeLibraryName = nil
        }

        var glesVersion: UInt32?
        var vulkanVersion: UInt32?
        var vulkanLevel: Int?
        var extensionPack = false
        var features: [String] = []
        for feature in root.children(named: "uses-feature") where feature.android(.required)?.boolean ?? true {
            if let version = feature.android(.glEsVersion)?.integer {
                glesVersion = max(glesVersion ?? 0, UInt32(truncatingIfNeeded: version))
            }
            guard let name = feature.android(.name)?.string else { continue }
            features.append(name)
            switch name {
            case "android.hardware.vulkan.version":
                vulkanVersion = feature.android(.version)?.integer.map { UInt32(truncatingIfNeeded: $0) }
            case "android.hardware.vulkan.level":
                vulkanLevel = feature.android(.version)?.integer
            case "android.hardware.opengles.aep":
                extensionPack = true
            default:
                break
            }
        }
        requiredGLESVersion = glesVersion
        requiredVulkanVersion = vulkanVersion
        requiredVulkanLevel = vulkanLevel
        requiresAndroidExtensionPack = extensionPack
        requiredFeatures = features

        permissions = root.children
            .filter { ["uses-permission", "uses-permission-sdk-23", "uses-permission-sdk-m"].contains($0.name) }
            .compactMap { $0.android(.name)?.string }
    }

    /// `.Foo` and `Foo` are relative to the package; anything with a dot inside is absolute.
    static func qualify(_ name: String, in package: String) -> String {
        if name.hasPrefix(".") { return package + name }
        if !name.contains(".") { return package + "." + name }
        return name
    }

    private static func sdkLevel(_ attribute: XMLAttribute?) -> Int? {
        guard let attribute else { return nil }
        if let level = attribute.integer { return level }
        // A codename such as "Baklava" marks a preview SDK.
        return attribute.string.map { _ in developmentSdk }
    }

    private static func launcher(in application: XMLElement, package: String) -> (className: String, element: XMLElement)? {
        let activities = application.children.filter { $0.name == "activity" }
        for component in application.children where component.name == "activity" || component.name == "activity-alias" {
            guard component.android(.enabled)?.boolean ?? true, isLauncher(component) else { continue }
            let isAlias = component.name == "activity-alias"
            guard let rawName = (isAlias ? component.android(.targetActivity) : component.android(.name))?.string else { continue }
            let className = qualify(rawName, in: package)
            let element = isAlias
                ? activities.first { $0.android(.name)?.string.map { qualify($0, in: package) } == className } ?? component
                : component
            return (className, element)
        }
        return nil
    }

    private static func isLauncher(_ component: XMLElement) -> Bool {
        component.children(named: "intent-filter").contains { filter in
            filter.children(named: "action").contains { $0.android(.name)?.string == "android.intent.action.MAIN" }
                && filter.children(named: "category").contains { $0.android(.name)?.string == "android.intent.category.LAUNCHER" }
        }
    }
}
