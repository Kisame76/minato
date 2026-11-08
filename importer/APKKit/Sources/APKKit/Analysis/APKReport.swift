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
    /// Whether the base's manifest requests android.permission.INTERNET within Android 15's bounds (ADR-0054 c2a); the
    /// start passes it to the runtime, whose sockets answer EPERM without it. Nil in reports from before the import read
    /// it, which count as granted, the runtime's default.
    public var usesInternet: Bool? = nil
    public var engine: String?
    /// The arm64 libraries that the app's Java code names in a string constant (DexStrings), sorted: what
    /// System.loadLibrary, System.load or an engine's list of libraries loads at the start (ADR-0055 decision 2). Nil
    /// in reports from before the import read the dex.
    public var javaLibraries: [String]? = nil
    /// Whether an APK of the app whose manifest lets it have code (android:hasCode, true by default) holds a
    /// classes.dex, classes2.dex, ...: Java code that a NativeActivity app reaches through JNI. Nil in reports from
    /// before the import looked, which count as none (each new Minato checks the library again, ADR-0055 decision 5).
    public var javaCode: Bool? = nil
    /// Path of the chosen icon image inside the APK.
    public var iconPath: String?
    public var signature: SignatureInfo
    public var compatibility: Compatibility
    /// Which Minato made this report, its analyzer and its converter's check (ADR-0055 decision 5): the library's check
    /// stamp at the import or the last check again. Nil in reports from before, which count as another Minato's.
    public var checkedBy: String? = nil
}

extension APKReport {
    /// Whether the app starts through app_process on the Java path: a Java app, and a NativeActivity app with its own
    /// Java code (ADR-0069 step 4a), whose JNI calls on activity-host would all answer that there is no Java. A
    /// NativeActivity app without code stays on activity-host until the parity rerun (ADR-0069 decision 3).
    public var startsOnJavaPath: Bool {
        if case .art = runtime { return true }
        return javaCode == true
    }
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
    /// The split APK that holds it in a split set (ADR-0048, decision 8); nil for the base, and in reports from before.
    public var split: String? = nil

    /// A file that is no ELF file (no ELF magic): Android's installer copies any lib/<abi>/lib*.so as a file whatever its
    /// contents, and a python-for-android app packs its Python bundle that way (libpybundle.so), so it is a plain file,
    /// not a library to check or convert.
    public var isPlainFile: Bool { elf == nil && problem?.contains("missing ELF magic") == true }
}

public struct Compatibility: Sendable, Equatable, Codable {
    /// What the import's library check used (ADR-0055 decision 5): the SHA-256 of the guest's export table and of its
    /// public.libraries.txt, so that a check after a Minato update can see that one changed. nil in reports from before,
    /// and where no check ran.
    public struct Checks: Sendable, Equatable, Codable {
        public var exportTable: String?
        public var publicLibraries: String?

        public init(exportTable: String? = nil, publicLibraries: String? = nil) {
            self.exportTable = exportTable
            self.publicLibraries = publicLibraries
        }
    }

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
        /// Android 15 refuses the app for its `<uses-sdk>` (INSTALL_FAILED_OLDER_SDK), with Android's message: a
        /// preview's codename, or a minSdk above 35.
        case olderSDK(String)
        /// The named `<uses-sdk>` attribute is a resource reference, which Android resolves and Minato does not
        /// (a logged departure, ADR-0039).
        case sdkReference(String)
        /// The set's splits offer nothing that fits this iPhone where it needs one, with the choice's reason (ADR-0048,
        /// decision 4).
        case splitChoice(String)
        // Degrading (ADR-0055 a2): the app runs, a part may fail when it is used.
        /// A library outside the start path that the converter's check refuses, with all its reasons one to a line: it
        /// is left out of lib/, so that System.loadLibrary throws UnsatisfiedLinkError as for a missing file.
        case libraryDegraded(library: String, reason: String)
        /// A library that stays and calls functions that only a stub of Minato's defines: they log once and answer
        /// nothing (ADR-0055 a4).
        case libraryCallsStubs(library: String, reason: String)
        // Informational (ADR-0055).
        /// A library whose check could not run, with the error's own text (a read error, a bad answer of the
        /// converter): it stays as it is, and the converter still refuses it at signing if it cannot run.
        case libraryNotChecked(library: String, reason: String)
        // Informational.
        /// Has Java/Kotlin code, so it needs ART (M4).
        case needsART
        /// Requires OpenGL ES newer than 3.1, which Minato offers on ANGLE's Vulkan backend (ADR-0013, ADR-0044 d));
        /// such an app runs there with 3.1.
        case requiresGLES(UInt32)
        /// Libraries linked for 4 KB pages only (ADR-0007). No longer reported: the converter lays them out for 16 KB
        /// pages at import, or refuses one with its own reason; kept so that older reports still decode.
        case pageAligned4K([String])
        /// Uses the Play licence check (LVL), which we do not work around.
        case usesLicenseCheck
        case unreadableLibrary(String)
    }

    public var status: Status
    public var issues: [Issue]
    public var checks: Checks? = nil
}
