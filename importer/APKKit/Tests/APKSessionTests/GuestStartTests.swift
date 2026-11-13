import APKLibrary
import Foundation
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
            data: "/v/data/p/data", deviceData: "/v/data/p/data_de", external: "/v/data/p/external",
            obb: "/v/data/p/obb", download: "/v/data/p/download",
            documents: "/v/data/p/documents", guestSystem: "/b/Minato.app/Guest",
            environment: environment)
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
            .init(guest: "/data/data/\(Self.package)", host: "/v/data/p/data"),
            .init(guest: "/data/user_de/0/\(Self.package)", host: "/v/data/p/data_de"),
            .init(guest: "/storage/emulated/0/Android/data/\(Self.package)", host: "/v/data/p/external"),
            .init(guest: "/storage/emulated/0/Android/obb/\(Self.package)", host: "/v/data/p/obb"),
            .init(guest: "/storage/emulated/0/Download", host: "/v/data/p/download"),
            .init(guest: "/storage/emulated/0/Documents", host: "/v/data/p/documents"),
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
                          localTesting: Bool = false, splits: [String] = [], environment: [String] = []) -> GuestStart? {
        GuestStart(
            javaApp: package, apk: "/v/apps/p/1/base.apk", apkSHA256: sha256, signers: signers,
            appLibraries: "/v/apps/p/1/lib", data: "/v/data/p/data", deviceData: "/v/data/p/data_de",
            external: "/v/data/p/external", obb: "/v/data/p/obb", download: "/v/data/p/download",
            documents: "/v/data/p/documents",
            guestData: "/c/guest/p/data",
            guestSystem: "/b/Minato.app/Guest", localTesting: localTesting, splits: splits, environment: environment)
    }

    /// An app whose dex the library compiled (ADR-0053, M6 c) finds its oat where ART looks for it, next to base.apk
    /// in oat/arm64: base.odex and base.vdex through a mount, and the odex's signed container as a library file of
    /// that folder.
    @Test func anAppWithCompiledCodeGetsItsOatFolderAndContainers() {
        let oat = GuestStart(
            javaApp: Self.package, apk: "/v/apps/p/1/base.apk", apkSHA256: Self.sha256, signers: nil,
            appLibraries: "/v/apps/p/1/lib", data: "/v/data/p/data", deviceData: "/v/data/p/data_de",
            external: "/v/data/p/external", obb: "/v/data/p/obb", download: "/v/data/p/download",
            documents: "/v/data/p/documents",
            guestData: "/c/guest/p/data", guestSystem: "/b/Minato.app/Guest",
            oat: .init(code: "/v/apps/p/1/oat", containers: "/v/apps/p/1/oat-containers"))
        #expect(oat?.mounts.contains(.init(guest: "\(Self.codePath)/oat/arm64", host: "/v/apps/p/1/oat")) == true)
        #expect(oat?.libraryDirectories.contains(
            .init(guest: "\(Self.codePath)/oat/arm64", host: "/v/apps/p/1/oat-containers")) == true)
        let plain = Self.javaStart()
        #expect(plain?.mounts.contains { $0.guest.hasSuffix("/oat/arm64") } == false)
        #expect(plain?.libraryDirectories.contains { $0.guest.hasSuffix("/oat/arm64") } == false)
    }

    // MARK: - A compile start (ADR-0053, M6 c): dex2oat for an app's dex

    static func compileStart() -> GuestStart? {
        GuestStart(compile: package, apk: "/v/apps/p/1/base.apk", apkSHA256: sha256, output: "/v/apps/p/1/oat.work",
                   guestSystem: "/b/Minato.app/Guest")
    }

    /// dex2oat as runtime/tests/dex2oat_make.py starts it for an app (framework_jar=): the app's whole boot class
    /// path, its class loader context, the filter speed, the APK at its code path as both the dex file and the
    /// location ART compares with the app's run-time path, and the output in the app's own /data/local/tmp, where the
    /// app may make files (the runtime's owners, as Android 15's: everything outside /data and /storage is root's).
    @Test func aCompileStartRunsDex2oatWithTheAppsBootClassPathAndContext() {
        let start = Self.compileStart()
        let bootClassPath = [
            "/apex/com.android.art/javalib/core-oj.jar", "/apex/com.android.art/javalib/core-libart.jar",
            "/apex/com.android.art/javalib/okhttp.jar", "/apex/com.android.art/javalib/bouncycastle.jar",
            "/system/framework/framework.jar", "/apex/com.android.art/javalib/core-icu4j.jar",
            "/apex/com.android.conscrypt/javalib/conscrypt.jar",
        ].joined(separator: ":")
        #expect(start?.program == "/b/Minato.app/Guest/bin/dex2oat.dylib")
        #expect(start?.arguments == [
            "/apex/com.android.art/bin/dex2oat64", "--dex-file=\(Self.codePath)/base.apk",
            "--dex-location=\(Self.codePath)/base.apk", "--oat-file=/data/local/tmp/base.odex",
            "--oat-location=/data/local/tmp/base.odex",
            "--instruction-set=arm64", "--compiler-filter=speed", "--base=0x70000000",
            "--class-loader-context=PCL[];PCL[]",
            "--runtime-arg", "-Xbootclasspath:\(bootClassPath)",
            "--runtime-arg", "-Xbootclasspath-locations:\(bootClassPath)",
            "--runtime-arg", "-Xnoimage-dex2oat",
        ])
    }

    @Test func aCompileStartMountsTheApkTheOutputAndTheSystem() {
        let start = Self.compileStart()
        #expect(start?.mounts == [
            .init(guest: "/data", host: "/v/apps/p/1/oat.work/data"),
            .init(guest: "\(Self.codePath)/base.apk", host: "/v/apps/p/1/base.apk"),
            .init(guest: "/system", host: "/b/Minato.app/Guest/system"),
            .init(guest: "/apex", host: "/b/Minato.app/Guest/apex"),
        ])
        #expect(start?.libraryDirectories == [
            .init(guest: "/system/lib64", host: "/b/Minato.app/Guest/lib64"),
            .init(guest: "/apex/com.android.art/lib64", host: "/b/Minato.app/Guest/apex-lib64/com.android.art"),
            .init(guest: "/apex/com.android.conscrypt/lib64",
                  host: "/b/Minato.app/Guest/apex-lib64/com.android.conscrypt"),
        ])
        #expect(Self.compileStart()?.codePath == Self.codePath)
    }

    @Test func aHashThatIsNotSHA256StartsNoCompile() {
        #expect(GuestStart(compile: Self.package, apk: "/a", apkSHA256: "", output: "/o", guestSystem: "/g") == nil)
    }

    // MARK: - The boot class path's checksum, which the build writes for the library's compiled code

    @Test func theBootClassPathChecksumIsTheBuildsFile() throws {
        let guest = FileManager.default.temporaryDirectory.appendingPathComponent("guest-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: guest, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: guest) }
        #expect(GuestStart.bootClassPathChecksum(guestSystem: guest.path) == nil)  // an older bundle has none
        let digest = String(repeating: "ab", count: 32)
        try Data("\(digest)\n".utf8).write(to: guest.appendingPathComponent("boot-class-path.sha256"))
        #expect(GuestStart.bootClassPathChecksum(guestSystem: guest.path) == digest)
        try Data("not a digest\n".utf8).write(to: guest.appendingPathComponent("boot-class-path.sha256"))
        #expect(GuestStart.bootClassPathChecksum(guestSystem: guest.path) == nil)
    }

    @Test func aJavaAppHasTheSameCodePath() {
        #expect(Self.javaStart()?.codePath == Self.codePath)
        #expect(Self.javaStart(sha256: "") == nil)
    }

    /// app_process64 with the harness's arguments: the manifest names the Application class (step c1.1), and the
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

    /// The guest's /data is the app's folder in Caches, where ART finds /data and its dalvik-cache (the
    /// decision for c5); Monet's overlays from the bundle lie over it as /data/resource-cache (ADR-0043 f)), and the
    /// app's paths, as in decision 14, with signers.json beside base.apk and ART's APEXes in /apex.
    @Test func mountsDataThenTheAppsPathsAndTheAPEXes() {
        let app = Self.codePath
        #expect(Self.javaStart()?.mounts == [
            .init(guest: "/data", host: "/c/guest/p/data"),
            .init(guest: "/data/resource-cache", host: "/b/Minato.app/Guest/resource-cache"),
            .init(guest: "\(app)/base.apk", host: "/v/apps/p/1/base.apk"),
            .init(guest: "\(app)/signers.json", host: "/v/apps/p/1/signers.json"),
            .init(guest: "/data/data/\(Self.package)", host: "/v/data/p/data"),
            .init(guest: "/data/user_de/0/\(Self.package)", host: "/v/data/p/data_de"),
            .init(guest: "/storage/emulated/0/Android/data/\(Self.package)", host: "/v/data/p/external"),
            .init(guest: "/storage/emulated/0/Android/obb/\(Self.package)", host: "/v/data/p/obb"),
            .init(guest: "/storage/emulated/0/Download", host: "/v/data/p/download"),
            .init(guest: "/storage/emulated/0/Documents", host: "/v/data/p/documents"),
            .init(guest: "/system", host: "/b/Minato.app/Guest/system"),
            .init(guest: "/apex", host: "/b/Minato.app/Guest/apex"),
        ])
    }

    /// An app whose kept pack slices the start lays out in local_testing of its external files folder gets
    /// ActivityThread's --local-testing-dir, as the harness passes it with --packs (ADR-0050 decision 2); an app without
    /// packs gets none. The mounts are the same either way, as the slices lie in the external folder's mount.
    @Test func anAppWithPacksGetsTheLocalTestingDir() {
        #expect(Self.javaStart(localTesting: true).map { Array($0.arguments.suffix(2)) } == ["--local-testing-dir", "local_testing"])
        #expect(Self.javaStart()?.arguments.contains("--local-testing-dir") == false)
        #expect(Self.javaStart(localTesting: true)?.mounts == Self.javaStart()?.mounts)
    }

    static let splits = ["/v/apps/p/1/split_feature.apk", "/v/apps/p/1/split_config.en.apk"]

    /// ADR-0048 decision 9: every APK of the set lies at the code path under its library name, mounted after
    /// base.apk, and ActivityThread gets one --split for each, in the order the set lists them.
    @Test func aSplitSetMountsEachSplitBesideBaseAndNamesItToActivityThread() {
        let app = Self.codePath
        let start = Self.javaStart(splits: Self.splits)
        #expect(start?.mounts.prefix(5).suffix(3) == [
            .init(guest: "\(app)/base.apk", host: "/v/apps/p/1/base.apk"),
            .init(guest: "\(app)/split_feature.apk", host: "/v/apps/p/1/split_feature.apk"),
            .init(guest: "\(app)/split_config.en.apk", host: "/v/apps/p/1/split_config.en.apk"),
        ])
        #expect(start?.arguments.prefix(10).suffix(6) == [
            "--apk", "\(app)/base.apk", "--split", "\(app)/split_feature.apk",
            "--split", "\(app)/split_config.en.apk",
        ])
        #expect(Self.javaStart()?.arguments.contains("--split") == false)
    }

    /// The same for a NativeActivity app: activity-host takes one --split for each, after --apk.
    @Test func aSplitSetNamesEachSplitToActivityHost() {
        let app = Self.codePath
        let start = GuestStart(
            package: Self.package, library: "activity-test", function: nil,
            apk: "/v/apps/p/1/base.apk", apkSHA256: Self.sha256, appLibraries: "/v/apps/p/1/lib",
            data: "/v/data/p/data", deviceData: "/v/data/p/data_de", external: "/v/data/p/external",
            obb: "/v/data/p/obb", download: "/v/data/p/download",
            documents: "/v/data/p/documents", guestSystem: "/b/Minato.app/Guest",
            splits: Self.splits)
        #expect(start?.mounts.prefix(3) == [
            .init(guest: "\(app)/base.apk", host: "/v/apps/p/1/base.apk"),
            .init(guest: "\(app)/split_feature.apk", host: "/v/apps/p/1/split_feature.apk"),
            .init(guest: "\(app)/split_config.en.apk", host: "/v/apps/p/1/split_config.en.apk"),
        ])
        #expect(start?.arguments.prefix(9).suffix(6) == [
            "--apk", "\(app)/base.apk", "--split", "\(app)/split_feature.apk",
            "--split", "\(app)/split_config.en.apk",
        ])
    }

    /// ADR-0054 c2a: a start holds INTERNET unless the session says otherwise from the app's report.
    @Test func aStartHoldsInternetByDefault() {
        #expect(Self.javaStart()?.internet == true)
        var start = Self.javaStart()
        start?.internet = false
        #expect(start?.internet == false)
        #expect(start?.arguments == Self.javaStart()?.arguments)
    }

    /// ADR-0065 and ADR-0066: without a profile the start sets neither the device profile nor an FPS unlock.
    @Test func aStartHasNoProfileByDefault() {
        #expect(Self.start()?.deviceProfile == nil)
        #expect(Self.start()?.frameRateUnlock == 0)
    }

    /// The app's profile names the runtime's device profile and unlock; nothing else of the start changes.
    @Test func theAppsProfileGivesTheDeviceAndTheUnlock() {
        var start = Self.start()
        start?.apply(AppProfile(deviceProfile: .galaxy, frameRate: .fps120), screenMaximum: 120)
        #expect(start?.deviceProfile == "galaxy")
        #expect(start?.frameRateUnlock == 120)
        #expect(start?.arguments == Self.start()?.arguments)
        #expect(start?.environment == Self.start()?.environment)

        start?.apply(AppProfile(), screenMaximum: 120)
        #expect(start == Self.start())
    }

    /// The user's grant to install apps reaches the runtime at the start; denied without it, as on Android.
    @Test func theAppsProfileGivesTheInstallGrant() {
        var start = Self.start()
        #expect(start?.installSources == false)
        start?.apply(AppProfile(installSources: true), screenMaximum: 120)
        #expect(start?.installSources == true)
        start?.apply(AppProfile(), screenMaximum: 120)
        #expect(start == Self.start())
    }

    /// A stored 120 on a screen that shows fewer runs at the screen's highest rate the unlock takes, never refused.
    @Test(arguments: [(120, 120, 120), (120, 60, 60), (120, 90, 60), (60, 120, 60), (60, 60, 60), (120, 30, 60)])
    func theUnlockIsAtMostTheScreensRate(stored: Int, screen: Int, unlock: Int32) {
        var start = Self.start()
        start?.apply(AppProfile(frameRate: AppProfile.FrameRate(rawValue: stored)), screenMaximum: screen)
        #expect(start?.frameRateUnlock == unlock)
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
