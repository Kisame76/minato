import Testing

@testable import APKSession

/// ADR-0032, step c, on ADR-0028 d): what game mode sets up before activity-host runs, as the header of
/// runtime/activity-host/activity_host.cpp asks (decision 14), and the configuration it connects with.
@Suite("GuestStart")
struct GuestStartTests {
    static let package = "io.github.kisame76.minato.test.activity"

    static func start(library: String = "activity-test", function: String? = nil,
                      environment: [String] = []) -> GuestStart {
        GuestStart(
            package: package, library: library, function: function,
            apk: "/v/apps/p/1/base.apk", appLibraries: "/v/apps/p/1/lib",
            data: "/v/data/p/data", external: "/v/data/p/external", obb: "/v/data/p/obb",
            guestSystem: "/b/Minato.app/Guest", environment: environment)
    }

    @Test func mountsThePathsOfDecision14() {
        let app = "/data/app/\(Self.package)"
        #expect(Self.start().mounts == [
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
        #expect(Self.start().libraryDirectories == [
            .init(guest: "/system/lib64", host: "/b/Minato.app/Guest/lib64"),
            .init(guest: "/data/app/\(Self.package)/lib/arm64", host: "/v/apps/p/1/lib"),
        ])
    }

    @Test func startsActivityHostWithTheAppsLibrary() {
        let start = Self.start(library: "main", function: "game_onCreate")
        #expect(start.program == "/b/Minato.app/Guest/bin/activity-host.dylib")
        #expect(start.arguments == [
            "/system/bin/activity-host", "--package", Self.package,
            "--apk", "/data/app/\(Self.package)/base.apk", "--library", "main", "--function", "game_onCreate",
        ])
        #expect(start.environment == ["LD_LIBRARY_PATH=/data/app/\(Self.package)/lib/arm64"])
    }

    /// V5 (ADR-0030, F15): a benchmark run's extra entries, such as libvulkan's MINATO_VULKAN_BGRA8_FIRST, come
    /// after LD_LIBRARY_PATH.
    @Test func addsTheExtraEnvironmentAfterTheLibraryPath() {
        #expect(Self.start(environment: ["MINATO_VULKAN_BGRA8_FIRST=1"]).environment == [
            "LD_LIBRARY_PATH=/data/app/\(Self.package)/lib/arm64", "MINATO_VULKAN_BGRA8_FIRST=1",
        ])
    }

    /// A report from before the import read `android.app.func_name` has none: Android's default.
    @Test func withoutAFunctionNameItIsAndroidsDefault() {
        #expect(Self.start(function: nil).arguments.suffix(2) == ["--function", "ANativeActivity_onCreate"])
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
