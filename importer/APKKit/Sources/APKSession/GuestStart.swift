import Foundation

/// What game mode sets up before activity-host runs (ADR-0028, decision 14; the header of
/// runtime/activity-host/activity_host.cpp), or app_process64 for a Java app (ADR-0039 c5): the guest paths of the
/// app and the system, the library folders, and the program with its arguments and environment.
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

    /// The installed version's folder in the guest, /data/app/~~<a>/<package>-<b> (ADR-0039, decision 6): the
    /// sourceDir's folder, with base.apk and lib/arm64.
    public let codePath: String
    /// For `apkrt_mount`, in order.
    public let mounts: [Path]
    /// For `apkrt_add_library_directory`: the system's containers, then the app's.
    public let libraryDirectories: [Path]
    /// The program's container: activity-host's, or app_process64's for a Java app.
    public let program: String
    public let arguments: [String]
    public let environment: [String]
    /// The backend under the session's ANGLE (ADR-0044 d)), from the installed set's glEsVersion; Metal by default.
    public var angleBackend: ANGLEBackend = .metal
    /// Whether the app holds android.permission.INTERNET, for `apkrt_set_internet` before the program starts (ADR-0054
    /// c2a): from the installed version's report, granted by default, as for a report from before the import read it.
    public var internet = true

    /// NativeActivity's default for `android.app.func_name`.
    public static let defaultFunction = "ANativeActivity_onCreate"

    /// `library` and `function` are the manifest's `android.app.lib_name` and `android.app.func_name`, the
    /// latter nil in a report from before the import read it. The host paths: the installed version's
    /// base.apk and lib/ (its containers), the app's data, device protected data, external, OBB and Download folders,
    /// and the guest system in the app's bundle (tools/embed-guest-system.sh), with system/, lib64/ and bin/.
    /// `apkSHA256` is base.apk's digest in hex, which names the code path; nil when it is no SHA-256. `environment` follows
    /// LD_LIBRARY_PATH: a benchmark run's entries (-benchmarkGuestEnvironment, ADR-0030 V5). `splits` are the host
    /// paths of an installed set's split APKs, `split_<name>.apk` in the version's folder (ADR-0048 decision 9): each
    /// lies beside base.apk at the code path under the same name, and activity-host gets one `--split` for each.
    public init?(package: String, library: String, function: String?, apk: String, apkSHA256: String,
                 appLibraries: String, data: String, deviceData: String, external: String, obb: String,
                 download: String, guestSystem: String, splits: [String] = [], environment extra: [String] = []) {
        guard let app = Self.codePath(package: package, apkSHA256: apkSHA256) else { return nil }
        codePath = app
        mounts = [Path(guest: "\(app)/base.apk", host: apk)] + Self.splitMounts(splits, app: app)
            + Self.dataMounts(package: package, data: data, deviceData: deviceData, external: external, obb: obb,
                             download: download)
            + [Path(guest: "/system", host: "\(guestSystem)/system")]
        libraryDirectories = [
            Path(guest: "/system/lib64", host: "\(guestSystem)/lib64"),
            Path(guest: "\(app)/lib/arm64", host: appLibraries),
        ]
        program = "\(guestSystem)/bin/activity-host.dylib"
        arguments = ["/system/bin/activity-host", "--package", package, "--apk", "\(app)/base.apk"]
            + Self.splitArguments(splits, app: app)
            + ["--library", library, "--function", function ?? Self.defaultFunction]
        environment = ["LD_LIBRARY_PATH=\(app)/lib/arm64"] + extra
    }

    /// A Java app's start (ADR-0039 c5): app_process64 with the arguments of framework/tests/app_process_run.py's
    /// start(), whose ActivityThread takes the Application class from the manifest. `signers` is the version's
    /// signers.json, nil for an unsigned APK. `guestData` stands for /data: the app's folder in Caches, where ART
    /// finds /data and its dalvik-cache (the lead's decision for c5); Monet's overlays from the bundle lie over it as
    /// /data/resource-cache, where every app's Resources find them (ADR-0043 f)), the app's paths too, and ART's
    /// APEXes are /apex. An app gets no LD_LIBRARY_PATH (ADR-0039, decision 2); `environment` is a run's extra
    /// entries. `localTesting`: the library keeps pack slices of the app, which the start lays out in local_testing of
    /// its external files folder (LibraryPaths.layOutPacks), so ActivityThread names that folder as the meta-data
    /// local_testing_dir (ADR-0050 decision 2). `splits` as for the NativeActivity start: ActivityThread gets one
    /// `--split` for each, after `--apk`.
    public init?(javaApp package: String, apk: String, apkSHA256: String, signers: String?, appLibraries: String,
                 data: String, deviceData: String, external: String, obb: String, download: String, guestData: String,
                 guestSystem: String, localTesting: Bool = false, splits: [String] = [],
                 environment extra: [String] = []) {
        guard let app = Self.codePath(package: package, apkSHA256: apkSHA256) else { return nil }
        codePath = app
        mounts = [Path(guest: "/data", host: guestData),
                  Path(guest: "/data/resource-cache", host: "\(guestSystem)/resource-cache"),
                  Path(guest: "\(app)/base.apk", host: apk)]
            + Self.splitMounts(splits, app: app)
            + (signers.map { [Path(guest: "\(app)/signers.json", host: $0)] } ?? [])
            + Self.dataMounts(package: package, data: data, deviceData: deviceData, external: external, obb: obb,
                             download: download)
            + [Path(guest: "/system", host: "\(guestSystem)/system"), Path(guest: "/apex", host: "\(guestSystem)/apex")]
        libraryDirectories = [
            Path(guest: "/system/lib64", host: "\(guestSystem)/lib64"),
            Path(guest: "/apex/com.android.art/lib64", host: "\(guestSystem)/apex-lib64/com.android.art"),
            Path(guest: "/apex/com.android.conscrypt/lib64", host: "\(guestSystem)/apex-lib64/com.android.conscrypt"),
            Path(guest: "\(app)/lib/arm64", host: appLibraries),
        ]
        program = "\(guestSystem)/bin/app_process64.dylib"
        arguments = ["/system/bin/app_process64", "--nice-name=\(package)", "--package", package,
                     "--apk", "\(app)/base.apk"]
            + Self.splitArguments(splits, app: app)
            + ["--library-path", "\(app)/lib/arm64", "--data-dir", "/data/user/0/\(package)"]
            + (signers == nil ? [] : ["--signers", "\(app)/signers.json"])
            + (localTesting ? ["--local-testing-dir", "local_testing"] : [])
        environment = extra
    }

    /// A split's file in the code path: the host file's own name, which the library gives `split_<name>.apk`.
    private static func splitMounts(_ splits: [String], app: String) -> [Path] {
        splits.map { Path(guest: "\(app)/\(($0 as NSString).lastPathComponent)", host: $0) }
    }

    private static func splitArguments(_ splits: [String], app: String) -> [String] {
        splitMounts(splits, app: app).flatMap { ["--split", $0.guest] }
    }

    /// The app's data, external data and OBB folders at the paths that Android 15's realpath gives them, /data/data and
    /// /storage/emulated/0 (decision 14 as changed by ADR-0050 c1's d4): the runtime's links lead /data/user/0 and
    /// /sdcard there, as Android 15's zygote and its storage links do. Its device protected data is
    /// /data/user_de/0/<package>, where Android 15 keeps it with no link, which createDeviceProtectedStorageContext's
    /// files and preferences name. Download is the app's own folder, where the host clones the files the user picks
    /// (ADR-0057 step b), as the harness's.
    private static func dataMounts(package: String, data: String, deviceData: String, external: String,
                                   obb: String, download: String) -> [Path] {
        [
            Path(guest: "/data/data/\(package)", host: data),
            Path(guest: "/data/user_de/0/\(package)", host: deviceData),
            Path(guest: "/storage/emulated/0/Android/data/\(package)", host: external),
            Path(guest: "/storage/emulated/0/Android/obb/\(package)", host: obb),
            Path(guest: "/storage/emulated/0/Download", host: download),
        ]
    }

    /// Android 15's form, from PackageManagerServiceUtils.getNextCodePath: "~~" and 16 bytes, then the package,
    /// "-" and 16 more, each in Base64 URL_SAFE | NO_WRAP, which keeps the padding. Android draws the bytes at
    /// random at each install; here they are the digest's halves, so a version keeps its path and an update
    /// moves it, as every install on Android does.
    static func codePath(package: String, apkSHA256: String) -> String? {
        guard let digest = bytes(hex: apkSHA256), digest.count == 32 else { return nil }
        return "/data/app/~~\(base64URL(digest.prefix(16)))/\(package)-\(base64URL(digest.suffix(16)))"
    }

    private static func bytes(hex: String) -> [UInt8]? {
        let digits = Array(hex.utf8)
        // UInt8(_:radix:) would take a sign too.
        guard digits.count.isMultiple(of: 2), hex.allSatisfy(\.isHexDigit) else { return nil }
        var bytes: [UInt8] = []
        for index in stride(from: 0, to: digits.count, by: 2) {
            guard let byte = UInt8(String(decoding: digits[index..<index + 2], as: UTF8.self), radix: 16) else {
                return nil
            }
            bytes.append(byte)
        }
        return bytes
    }

    private static func base64URL(_ bytes: ArraySlice<UInt8>) -> String {
        Data(bytes).base64EncodedString().replacingOccurrences(of: "+", with: "-").replacingOccurrences(of: "/", with: "_")
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
