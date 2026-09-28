/// What game mode sets up before activity-host runs (ADR-0028, decision 14; the header of
/// runtime/activity-host/activity_host.cpp): the guest paths of the app and the system, the library
/// folders, and the program with its arguments and environment.
public struct GuestStart: Sendable, Equatable {
    /// A guest path and the host path it stands for.
    public struct Path: Sendable, Equatable {
        public let guest: String
        public let host: String

        public init(guest: String, host: String) {
            self.guest = guest
            self.host = host
        }
    }

    /// For `apkrt_mount`, in order.
    public let mounts: [Path]
    /// For `apkrt_add_library_directory`: the system's containers, then the app's.
    public let libraryDirectories: [Path]
    /// activity-host's container.
    public let program: String
    public let arguments: [String]
    public let environment: [String]

    /// `library` is the manifest's `android.app.lib_name`; activity-host looks up ANativeActivity_onCreate,
    /// since the import does not read `android.app.func_name` yet. The host paths: the installed version's
    /// base.apk and lib/ (its containers), the app's data, external and OBB folders, and the guest system in
    /// the app's bundle (tools/embed-guest-system.sh), with system/, lib64/ and bin/.
    public init(package: String, library: String, apk: String, appLibraries: String, data: String, external: String,
                obb: String, guestSystem: String) {
        let app = "/data/app/\(package)"
        mounts = [
            Path(guest: "\(app)/base.apk", host: apk),
            Path(guest: "/data/user/0/\(package)", host: data),
            Path(guest: "/data/data/\(package)", host: data),
            Path(guest: "/storage/emulated/0/Android/data/\(package)", host: external),
            Path(guest: "/sdcard/Android/data/\(package)", host: external),
            Path(guest: "/storage/emulated/0/Android/obb/\(package)", host: obb),
            Path(guest: "/sdcard/Android/obb/\(package)", host: obb),
            Path(guest: "/system", host: "\(guestSystem)/system"),
        ]
        libraryDirectories = [
            Path(guest: "/system/lib64", host: "\(guestSystem)/lib64"),
            Path(guest: "\(app)/lib/arm64", host: appLibraries),
        ]
        program = "\(guestSystem)/bin/activity-host.dylib"
        arguments = ["/system/bin/activity-host", "--package", package, "--apk", "\(app)/base.apk", "--library", library]
        environment = ["LD_LIBRARY_PATH=\(app)/lib/arm64"]
    }
}

/// What activity-host gets when it connects (`struct apkrt_configuration`, ADR-0028, decision 3).
public struct GuestConfiguration: Sendable, Equatable {
    /// The screen's native scale.
    public let scale: Double
    /// The screen's size in pixels, in the session's orientation.
    public let width: Int32
    public let height: Int32
    /// ACONFIGURATION_ORIENTATION_PORT (1) or _LAND (2).
    public let orientation: Int32
    /// ACONFIGURATION_UI_MODE_NIGHT_NO (1) or _YES (2).
    public let night: Int32
    /// The display's highest rate in frames per second.
    public let refreshRate: Int32
    /// ISO 639, two or three lowercase letters, or empty.
    public let language: String
    /// ISO 3166, two uppercase letters, or empty.
    public let country: String

    public init(scale: Double, width: Int32, height: Int32, dark: Bool, refreshRate: Int32, languageCode: String?,
                regionCode: String?) {
        self.scale = scale
        self.width = width
        self.height = height
        orientation = width > height ? 2 : 1
        night = dark ? 2 : 1
        self.refreshRate = refreshRate
        // AConfiguration holds two letters for the country and two or three for the language.
        language = Self.letters(languageCode, counts: 2...3)?.lowercased() ?? ""
        country = Self.letters(regionCode, counts: 2...2)?.uppercased() ?? ""
    }

    private static func letters(_ code: String?, counts: ClosedRange<Int>) -> String? {
        guard let code, counts.contains(code.count), code.allSatisfy({ $0.isASCII && $0.isLetter }) else { return nil }
        return code
    }
}
