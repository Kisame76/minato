import Foundation

/// Everything the library and later the runtime need to know about one APK.
public struct APKReport: Sendable, Equatable, Codable {
    public enum Runtime: Sendable, Equatable, Codable {
        /// Launched through android.app.NativeActivity: runs without ART (M3).
        case nativeActivity(library: String)
        /// Needs ART and the Java framework (M4).
        case art
    }

    public enum PlayDependency: String, Sendable, Codable, CaseIterable {
        case playServices
        case firebase
        case billing
        case licenseCheck
        case playGames
    }

    public var packageName: String
    public var label: String
    public var versionCode: Int64
    public var versionName: String?
    public var minSdk: Int
    public var targetSdk: Int
    public var launcherActivity: String?
    /// The launcher activity's `android:screenOrientation`, one of ActivityInfo's constants; nil when
    /// unset. Game mode holds the screen by it (ADR-0032).
    public var screenOrientation: Int?
    /// The launcher activity's `android:configChanges`, ActivityInfo's CONFIG_ flags; nil when unset or in
    /// reports from before the import read it. Whether a session turns depends on it (ADR-0038).
    public var configChanges: Int? = nil
    public var runtime: Runtime
    /// For a NativeActivity, the function it calls (`android.app.func_name`); nil in reports from before
    /// the import read it, which then mean Android's default.
    public var nativeFunction: String?
    /// ABIs with native libraries, sorted.
    public var abis: [String]
    /// "arm64-v8a" when present; the only ABI an iPhone can run (ADR-0006).
    public var selectedABI: String?
    /// Libraries of the selected ABI, in archive order.
    public var nativeLibraries: [NativeLibrary]
    public var extractNativeLibs: Bool?
    public var requiredGLESVersion: UInt32?
    public var requiredVulkanVersion: UInt32?
    public var requiredVulkanLevel: Int?
    public var requiresAndroidExtensionPack: Bool
    public var splitName: String?
    public var requiresSplits: Bool
    public var playDependencies: [PlayDependency]
    public var engine: String?
    /// Path of the chosen icon image inside the APK.
    public var iconPath: String?
    public var signature: SignatureInfo
    public var compatibility: Compatibility
}

public struct NativeLibrary: Sendable, Equatable, Codable {
    public var name: String
    public var path: String
    public var size: UInt64
    public var isCompressed: Bool
    /// For stored libraries: whether the data starts on a 16 KB boundary (needed to map them in place).
    public var isZipAligned16K: Bool?
    public var elf: ELFInfo?
    /// Why the ELF header could not be read, if it could not.
    public var problem: String?
}

public struct Compatibility: Sendable, Equatable, Codable {
    public enum Status: String, Sendable, Codable {
        case ready
        case needsSplits
        case incompatible
    }

    public enum Issue: Sendable, Equatable, Hashable, Codable {
        // Blocking.
        /// The file is a split (config or feature) and cannot be installed on its own.
        case isSplitAPK(String)
        /// Native code only for 32-bit ARM, which iPhones cannot execute.
        case only32Bit([String])
        /// Native code, but none for ARM.
        case noARM64([String])
        /// A library in lib/arm64-v8a is not AArch64 code.
        case wrongArchitecture(String)
        /// The converter refuses a library: it cannot run on iOS as it is (ADR-0010, ADR-0032).
        case conversionFailed(library: String, reason: String)
        /// The signature check failed: the file is damaged or forged (ADR-0034, step d).
        case signatureInvalid(SignatureFailure)
        // Informational.
        /// Has Java/Kotlin code, so it needs ART (M4).
        case needsART
        /// Requires OpenGL ES newer than 3.0, which ANGLE's Metal backend does not offer (ADR-0013).
        case requiresGLES(UInt32)
        /// Libraries linked for 4 KB pages only (ADR-0007).
        case pageAligned4K([String])
        /// Uses the Play licence check (LVL), which we do not work around.
        case usesLicenseCheck
        case unreadableLibrary(String)
    }

    public var status: Status
    public var issues: [Issue]
}
