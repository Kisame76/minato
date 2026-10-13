import Foundation

/// The parts of AndroidManifest.xml that import and compatibility checks need.
/// Resource references (label, icon, versionName) stay unresolved here; `APKAnalyzer` resolves them.
public struct AndroidManifest: Sendable, Equatable {
    public var packageName: String
    public var versionCode: Int64
    public var versionName: ResourceValue?
    public var minSdk: Int
    public var targetSdk: Int
    /// Android's message when Android 15 refuses to install the app for its `<uses-sdk>`.
    public var sdkRefusal: String?
    /// The `<uses-sdk>` attribute that names a resource, which Android resolves and Minato does not.
    public var sdkReference: String?

    /// Set when this APK is itself a split (`<manifest split="config.arm64_v8a">`); an empty name is the base.
    public var splitName: String?
    /// The module a configuration split configures (`configForSplit`).
    public var configForSplit: String?
    public var isFeatureSplit: Bool
    /// `<manifest android:isolatedSplits>`: only where the base sets it does Android build the splits' dependency tree,
    /// and check it.
    public var isolatedSplits: Bool
    /// The first `<uses-split>`; Android ignores the others.
    public var usesSplit: String?
    /// `requiredSplitTypes` and `splitTypes` as Android splits and checks them.
    public var requiredSplitTypes: Set<String>
    public var splitTypes: Set<String>
    /// `<manifest android:isSplitRequired>`, which Android 15 reads.
    public var splitRequired: Bool
    /// What bundletool wrote and Android 15 ignores: `<application android:isSplitRequired>` or the meta-data
    /// `com.android.vending.splits.required` (F1). Minato reads them as "Needs split APKs" too.
    public var stricterSplitSignal: Bool
    /// The base APK cannot run without its splits: it says so in one of the three ways, or names split types.
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
    /// The launcher activity's `android:screenOrientation`, one of ActivityInfo's constants; nil when unset.
    public var screenOrientation: Int?
    /// The launcher activity's `android:configChanges`, ActivityInfo's CONFIG_ flags; nil when unset.
    public var configChanges: Int?
    public var usesNativeActivity: Bool
    /// Library that NativeActivity loads (`android.app.lib_name`, default "main").
    public var nativeLibraryName: String?
    /// Function in it that NativeActivity calls (`android.app.func_name`, default "ANativeActivity_onCreate").
    public var nativeFunctionName: String?

    public var requiredGLESVersion: UInt32?
    public var requiredVulkanVersion: UInt32?
    public var requiredVulkanLevel: Int?
    public var requiresAndroidExtensionPack: Bool
    public var requiredFeatures: [String]

    public var permissions: [String]
    /// Whether Android 15's package parser requests android.permission.INTERNET (ADR-0054 c2a): a <uses-permission>,
    /// <uses-permission-sdk-23> or <uses-permission-sdk-m> of that name whose int-typed minSdkVersion and maxSdkVersion,
    /// where set, include 35, as ParsingPackageUtils reads them. Its required-feature conditions are not checked
    /// [departure: ManifestPackage checks them for the guest's permissions].
    public var requestsInternet = false
    public var metaData: [String: ResourceValue]
    public var usesLibraries: [String]
    /// Qualified names of activities, aliases, services, receivers and providers.
    public var components: [String]

    /// A codename's level, Build.VERSION_CODES.CUR_DEVELOPMENT, as a preview build gives it.
    public static let developmentSdk = 10_000

    public init(xml root: XMLElement) throws {
        guard root.name == "manifest" else { throw APKError.invalidManifest("root element is <\(root.name)>") }
        // Android's installer reads the identity first, by namespace and name, and refuses what it cannot take
        // (ApkLiteParseUtils.java:406-440, 685-773; ADR-0048 b2). Its package parser and the guest read package and
        // split the same way, and no other reading takes the split attributes (PackageImpl.java:2720-2736).
        let installer = InstallerReading(root)
        let package: String, splitText: String?
        do {
            guard let text = try installer.text("package") else { throw APKError.invalidManifest("missing package name") }
            package = text
            if package != "android",
               let problem = InstallerReading.validateName(package, requireSeparator: true, requireFilename: true) {
                throw APKError.invalidManifest("Invalid manifest package: \(problem)")
            }
            splitText = try installer.text("split")
            if let splitText, !splitText.isEmpty,
               let problem = InstallerReading.validateName(splitText, requireSeparator: false, requireFilename: false) {
                throw APKError.invalidManifest("Invalid manifest split: \(problem)")
            }
            requiredSplitTypes = try InstallerReading.splitTypes(installer.text("requiredSplitTypes", android: true))
            splitTypes = try InstallerReading.splitTypes(installer.text("splitTypes", android: true))
            configForSplit = try installer.text("configForSplit")
            usesSplit = nil
            if let element = root.children(named: "uses-split").first {
                guard let name = try InstallerReading(element).text("name", android: true) else {
                    throw APKError.invalidManifest("<uses-split> tag requires 'android:name' attribute")
                }
                usesSplit = name
            }
        } catch let unreadable as InstallerReading.Unreadable {
            throw APKError.invalidManifest("Not a valid APK: Android cannot read the manifest's \(unreadable.name)")
        }
        packageName = package
        splitName = splitText?.isEmpty == true ? nil : splitText

        // The installer's flags are integer types' data (getAttributeBooleanValue).
        versionName = root.android(.versionName)?.value
        isFeatureSplit = installer.integer("isFeatureSplit").map { $0 != 0 } ?? false
        isolatedSplits = installer.integer("isolatedSplits").map { $0 != 0 } ?? false
        splitRequired = installer.integer("isSplitRequired").map { $0 != 0 } ?? false

        let sdk = try UsesSdk(manifest: root)
        minSdk = sdk.minSdk
        targetSdk = sdk.targetSdk
        sdkRefusal = sdk.refusal
        sdkReference = sdk.reference

        // The installer reads the versions by name (getAttributeIntValue), the package parser and the guest by resource
        // ID (PackageImpl.java:2721-2723). Where the two readings differ, the library would key what the guest does not
        // see (ADR-0048 decision 2). Where obfuscation renamed one, only the resource ID finds it, and that reading
        // stands. `versionCodeMajor` takes the upper 32 bits, as the session compares them.
        let parsedMinor = try Self.packageParserInteger(.versionCode, in: root)
        let parsedMajor = try Self.packageParserInteger(.versionCodeMajor, in: root)
        let minor = installer.find("versionCode", android: true) == nil
            ? parsedMinor : installer.integer("versionCode") ?? 0
        let major = installer.find("versionCodeMajor", android: true) == nil
            ? parsedMajor : installer.integer("versionCodeMajor") ?? 0
        func agree(_ name: String, _ installer: String, _ parser: String) throws {
            guard installer == parser else {
                throw APKError.invalidManifest("Not a valid APK: its manifest's \(name) reads as \(installer) and as \(parser)")
            }
        }
        try agree("versionCode", "\(UInt32(bitPattern: minor))", "\(UInt32(bitPattern: parsedMinor))")
        try agree("versionCodeMajor", "\(major)", "\(parsedMajor)")
        versionCode = Int64(major) << 32 | Int64(UInt32(bitPattern: minor))

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

        // bundletool's marker, which Android does not read: integer types only, as Android reads the <manifest> flag.
        let splitsFlag = application?.android(.isSplitRequired)
            .map { (0x10...0x1F).contains($0.dataType) && $0.data != 0 } ?? false
        let splitsMetaData: Bool = switch metaData["com.android.vending.splits.required"] {
        case .boolean(let flag): flag
        case .string(let text): text.lowercased() == "true"
        default: false
        }
        stricterSplitSignal = splitsFlag || splitsMetaData
        requiresSplits = splitRequired || !requiredSplitTypes.isEmpty || stricterSplitSignal

        let launcher = application.flatMap { Self.launcher(in: $0, package: package) }
        launcherActivity = launcher?.className
        screenOrientation = launcher?.element.android(.screenOrientation)?.integer
        configChanges = launcher?.element.android(.configChanges)?.integer
        usesNativeActivity = launcher?.className == "android.app.NativeActivity"
        if usesNativeActivity {
            // NativeActivity.onCreate reads both from the launcher's meta-data, with these defaults.
            let value = { (name: String) in
                launcher?.element.children(named: "meta-data")
                    .first { $0.android(.name)?.string == name }?
                    .android(.value)?.string
            }
            nativeLibraryName = value("android.app.lib_name") ?? "main"
            nativeFunctionName = value("android.app.func_name") ?? "ANativeActivity_onCreate"
        } else {
            nativeLibraryName = nil
            nativeFunctionName = nil
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

        let usesPermissions = root.children
            .filter { ["uses-permission", "uses-permission-sdk-23", "uses-permission-sdk-m"].contains($0.name) }
        permissions = usesPermissions.compactMap { $0.android(.name)?.string }
        requestsInternet = usesPermissions.contains { element in
            // An SDK bound counts only with an int type (TypedValue.TYPE_FIRST_INT to TYPE_LAST_INT).
            func bound(_ attribute: AndroidAttribute) -> Int? {
                guard let value = element.android(attribute), (0x10...0x1F).contains(value.dataType) else { return nil }
                return Int(Int32(bitPattern: value.data))
            }
            return element.android(.name)?.string == "android.permission.INTERNET"
                && (bound(.minSdkVersion) ?? 0) <= 35 && 35 <= (bound(.maxSdkVersion) ?? Int.max)
        }
    }

    /// An integer as Android's package parser reads it (`TypedArray.getInteger`, TypedArray.java:628-652), by resource
    /// ID alone: none, TYPE_NULL or @null is 0, an integer type gives its data, and anything else throws there, failing
    /// Android's parse. Android resolves a reference through the app's resources, which Minato does not read here, so
    /// one is refused too [departure].
    private static func packageParserInteger(_ attribute: AndroidAttribute, in root: XMLElement) throws -> Int32 {
        guard let id = attribute.id, let value = root.attribute(resourceID: id) else { return 0 }
        switch value.dataType {
        case 0x00:
            return 0
        case 0x01, 0x07:
            guard value.data == 0 else {
                throw APKError.invalidManifest(
                    "Not a valid APK: its manifest's \(attribute.name) names a resource, which Minato does not resolve")
            }
            return 0
        case 0x10...0x1F:
            return Int32(bitPattern: value.data)
        default:
            throw APKError.invalidManifest("Not a valid APK: Android cannot read the manifest's \(attribute.name)")
        }
    }

    /// `.Foo` and `Foo` are relative to the package; anything with a dot inside is absolute.
    static func qualify(_ name: String, in package: String) -> String {
        if name.hasPrefix(".") { return package + name }
        if !name.contains(".") { return package + "." + name }
        return name
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
