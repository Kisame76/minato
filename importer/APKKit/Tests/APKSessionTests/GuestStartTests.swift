import Testing

@testable import APKSession

/// ADR-0032, step c, on ADR-0028 d): what game mode sets up before activity-host runs, as the header of
/// runtime/activity-host/activity_host.cpp asks (decision 14), and the configuration it connects with.
@Suite("GuestStart")
struct GuestStartTests {
    static let package = "io.github.kisame76.minato.test.activity"
    /// base.apk's SHA-256: its first 16 bytes give "~~<a>", its last 16 "<package>-<b>", each in Base64 URL_SAFE
    /// with its padding, as PackageManagerServiceUtils.getNextCodePath writes Android's random bytes.
    static let sha256 = "fbffbf00000000000000000000000000101112131415161718191a1b1c1d1e1f"
    static let codePath = "/data/app/~~-_-_AAAAAAAAAAAAAAAAAA==/\(package)-EBESExQVFhcYGRobHB0eHw=="

    static func start(library: String = "activity-test", function: String? = nil, sha256: String = Self.sha256,
                      environment: [String] = []) -> GuestStart? {
        GuestStart(
            package: package, library: library, function: function,
            apk: "/v/apps/p/1/base.apk", apkSHA256: sha256, appLibraries: "/v/apps/p/1/lib",
            data: "/v/data/p/data", external: "/v/data/p/external", obb: "/v/data/p/obb",
            guestSystem: "/b/Minato.app/Guest", environment: environment)
    }

    /// ADR-0039, decision 6: Android 15's code path, /data/app/~~<a>/<package>-<b>.
    @Test func theCodePathIsAndroid15s() {
        #expect(Self.start()?.codePath == Self.codePath)
    }

    /// Android draws new bytes at each install; ours come from the version, so an update moves the path.
    @Test func eachVersionHasItsOwnCodePath() {
        let other = String(repeating: "ab", count: 32)
        #expect(Self.start(sha256: other)?.codePath
            == "/data/app/~~q6urq6urq6urq6urq6urqw==/\(Self.package)-q6urq6urq6urq6urq6urqw==")
        #expect(Self.start()?.codePath == Self.start()?.codePath)
        #expect(Self.start(sha256: Self.sha256.uppercased())?.codePath == Self.codePath)
    }

    @Test func aHashThatIsNotSHA256StartsNothing() {
        #expect(Self.start(sha256: "") == nil)
        #expect(Self.start(sha256: String(Self.sha256.dropLast(2))) == nil)
        #expect(Self.start(sha256: String(Self.sha256.dropLast(2)) + "zz") == nil)
    }

    @Test func mountsThePathsOfDecision14() {
        let app = Self.codePath
        #expect(Self.start()?.mounts == [
            .init(guest: "\(app)/base.apk", host: "/v/apps/p/1/base.apk"),
            .init(guest: "/data/user/0/\(Self.package)", host: "/v/data/p/data"),
            .init(guest: "/data/data/\(Self.package)", host: "/v/data/p/data"),
            .init(guest: "/storage/emulated/0/Android/data/\(Self.package)", host: "/v/data/p/external"),
            .init(guest: "/sdcard/Android/data/\(Self.package)", host: "/v/data/p/external"),
            .init(guest: "/storage/emulated/0/Android/obb/\(Self.package)", host: "/v/data/p/obb"),
            .init(guest: "/sdcard/Android/obb/\(Self.package)", host: "/v/data/p/obb"),
            .init(guest: "/system", host: "/b/Minato.app/Guest/system"),
        ])
    }

    @Test func findsTheSystemsLibrariesThenTheApps() {
        #expect(Self.start()?.libraryDirectories == [
            .init(guest: "/system/lib64", host: "/b/Minato.app/Guest/lib64"),
            .init(guest: "\(Self.codePath)/lib/arm64", host: "/v/apps/p/1/lib"),
        ])
    }

    @Test func startsActivityHostWithTheAppsLibrary() {
        let start = Self.start(library: "main", function: "game_onCreate")
        #expect(start?.program == "/b/Minato.app/Guest/bin/activity-host.dylib")
        #expect(start?.arguments == [
            "/system/bin/activity-host", "--package", Self.package,
            "--apk", "\(Self.codePath)/base.apk", "--library", "main", "--function", "game_onCreate",
        ])
        #expect(start?.environment == ["LD_LIBRARY_PATH=\(Self.codePath)/lib/arm64"])
    }

    /// V5 (ADR-0030, F15): a benchmark run's extra entries, such as libvulkan's MINATO_VULKAN_BGRA8_FIRST, come
    /// after LD_LIBRARY_PATH.
    @Test func addsTheExtraEnvironmentAfterTheLibraryPath() {
        #expect(Self.start(environment: ["MINATO_VULKAN_BGRA8_FIRST=1"])?.environment == [
            "LD_LIBRARY_PATH=\(Self.codePath)/lib/arm64", "MINATO_VULKAN_BGRA8_FIRST=1",
        ])
    }

    /// A report from before the import read `android.app.func_name` has none: Android's default.
    @Test func withoutAFunctionNameItIsAndroidsDefault() {
        #expect(Self.start(function: nil)?.arguments.suffix(2) == ["--function", "ANativeActivity_onCreate"])
    }

    // MARK: - A Java app (ADR-0039 c5), as framework/tests/app_process_run.py's start() starts it on the Mac

    static func javaStart(signers: String? = "/v/apps/p/1/signers.json", sha256: String = Self.sha256,
                          environment: [String] = []) -> GuestStart? {
        GuestStart(
            javaApp: package, apk: "/v/apps/p/1/base.apk", apkSHA256: sha256, signers: signers,
            appLibraries: "/v/apps/p/1/lib", data: "/v/data/p/data", external: "/v/data/p/external",
            obb: "/v/data/p/obb", guestData: "/c/guest/p/data", guestSystem: "/b/Minato.app/Guest",
            environment: environment)
    }

    @Test func aJavaAppHasTheSameCodePath() {
        #expect(Self.javaStart()?.codePath == Self.codePath)
        #expect(Self.javaStart(sha256: "") == nil)
    }

    /// app_process64 with the harness's arguments: the manifest names the Application class (App's c1.1), and the
    /// version's signers.json gives PackageManager the app's signatures (ADR-0039 b3.5b1).
    @Test func startsAppProcessWithTheHarnesssArguments() {
        let start = Self.javaStart()
        #expect(start?.program == "/b/Minato.app/Guest/bin/app_process64.dylib")
        #expect(start?.arguments == [
            "/system/bin/app_process64", "--nice-name=\(Self.package)", "--package", Self.package,
            "--apk", "\(Self.codePath)/base.apk", "--library-path", "\(Self.codePath)/lib/arm64",
            "--data-dir", "/data/user/0/\(Self.package)", "--signers", "\(Self.codePath)/signers.json",
        ])
    }

    /// The guest's /data is the app's folder in Caches, where ART finds /data and its dalvik-cache (the lead's
    /// decision for c5); Monet's overlays from the bundle lie over it as /data/resource-cache (ADR-0043 f)), and the
    /// app's paths, as in decision 14, with signers.json beside base.apk and ART's APEXes in /apex.
    @Test func mountsDataThenTheAppsPathsAndTheAPEXes() {
        let app = Self.codePath
        #expect(Self.javaStart()?.mounts == [
            .init(guest: "/data", host: "/c/guest/p/data"),
            .init(guest: "/data/resource-cache", host: "/b/Minato.app/Guest/resource-cache"),
            .init(guest: "\(app)/base.apk", host: "/v/apps/p/1/base.apk"),
            .init(guest: "\(app)/signers.json", host: "/v/apps/p/1/signers.json"),
            .init(guest: "/data/user/0/\(Self.package)", host: "/v/data/p/data"),
            .init(guest: "/data/data/\(Self.package)", host: "/v/data/p/data"),
            .init(guest: "/storage/emulated/0/Android/data/\(Self.package)", host: "/v/data/p/external"),
            .init(guest: "/sdcard/Android/data/\(Self.package)", host: "/v/data/p/external"),
            .init(guest: "/storage/emulated/0/Android/obb/\(Self.package)", host: "/v/data/p/obb"),
            .init(guest: "/sdcard/Android/obb/\(Self.package)", host: "/v/data/p/obb"),
            .init(guest: "/system", host: "/b/Minato.app/Guest/system"),
            .init(guest: "/apex", host: "/b/Minato.app/Guest/apex"),
        ])
    }

    /// An unsigned APK has no signers.json: no mount and no argument, as the harness starts it.
    @Test func anUnsignedJavaAppHasNoSigners() {
        let start = Self.javaStart(signers: nil)
        #expect(start?.arguments.contains("--signers") == false)
        #expect(start?.mounts.contains { $0.guest.hasSuffix("/signers.json") } == false)
    }

    /// The system's libraries, ART's and Conscrypt's in their APEXes, then the app's JNI libraries.
    @Test func findsTheSystemsAndTheAPEXesLibrariesThenTheApps() {
        #expect(Self.javaStart()?.libraryDirectories == [
            .init(guest: "/system/lib64", host: "/b/Minato.app/Guest/lib64"),
            .init(guest: "/apex/com.android.art/lib64", host: "/b/Minato.app/Guest/apex-lib64/com.android.art"),
            .init(guest: "/apex/com.android.conscrypt/lib64",
                  host: "/b/Minato.app/Guest/apex-lib64/com.android.conscrypt"),
            .init(guest: "\(Self.codePath)/lib/arm64", host: "/v/apps/p/1/lib"),
        ])
    }

    /// An app gets no LD_LIBRARY_PATH (ADR-0039 decision 2): only the extra entries.
    @Test func aJavaAppGetsOnlyTheExtraEnvironment() {
        #expect(Self.javaStart()?.environment == [])
        #expect(Self.javaStart(environment: ["MINATO_VULKAN_BGRA8_FIRST=1"])?.environment
            == ["MINATO_VULKAN_BGRA8_FIRST=1"])
    }

    @Test func theConfigurationHasTheScreenInTheSessionsOrientation() {
        let portrait = GuestConfiguration(scale: 3, width: 1290, height: 2796, dark: false, refreshRate: 120,
                                          languageCode: "de", regionCode: "DE")
        #expect(portrait.orientation == 1)
        #expect(portrait.night == 1)
        #expect(portrait.language == "de")
        #expect(portrait.country == "DE")

        let landscape = GuestConfiguration(scale: 3, width: 2796, height: 1290, dark: true, refreshRate: 60,
                                           languageCode: "en", regionCode: nil)
        #expect(landscape.orientation == 2)
        #expect(landscape.night == 2)
        #expect(landscape.country == "")
    }

    /// AConfiguration keeps two letters for the country (ISO 3166) and two or three for the language
    /// (ISO 639): a region like "419" or a longer code does not fit, and the guest gets none.
    @Test func keepsOnlyCodesThatFit() {
        let configuration = GuestConfiguration(scale: 2, width: 750, height: 1334, dark: false, refreshRate: 60,
                                               languageCode: "fil", regionCode: "419")
        #expect(configuration.language == "fil")
        #expect(configuration.country == "")
        #expect(GuestConfiguration(scale: 2, width: 1, height: 2, dark: false, refreshRate: 60,
                                   languageCode: "yue-Hant", regionCode: "hk").language == "")
        #expect(GuestConfiguration(scale: 2, width: 1, height: 2, dark: false, refreshRate: 60,
                                   languageCode: nil, regionCode: "hk").country == "HK")
    }
}
