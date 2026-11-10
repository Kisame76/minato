import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// The per-app profile (ADR-0065 decision 1, ADR-0066 decision 2): the user's choices for one app in its data folder.
@Suite("App profile")
struct AppProfileTests {
    typealias Sandbox = LibraryTests.Sandbox

    static let package = "org.example.game"

    @Test func anAppWithoutAFileHasTheDefault() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }

        #expect(AppProfile.load(for: Self.package, in: sandbox.paths) == AppProfile())
        #expect(AppProfile().isDefault)
    }

    @Test func aSavedProfileIsReadBack() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let profile = AppProfile(deviceProfile: .galaxy, frameRate: .fps120)

        try profile.save(for: Self.package, in: sandbox.paths)

        #expect(AppProfile.load(for: Self.package, in: sandbox.paths) == profile)
        #expect(sandbox.paths.profileFile(for: Self.package)
            == sandbox.paths.dataDirectory(for: Self.package).appendingPathComponent("profile.json"))
    }

    /// Whether the app may install apps (REQUEST_INSTALL_PACKAGES), which the user allows when the app asks: kept with
    /// the profile, off where a file from before has no key.
    @Test func theInstallSourcesGrantIsKeptAndOffByDefault() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let old = Data(#"{"deviceProfile":"galaxy"}"#.utf8)
        try FileManager.default.createDirectory(at: sandbox.paths.dataDirectory(for: Self.package),
                                                withIntermediateDirectories: true)
        try old.write(to: sandbox.paths.profileFile(for: Self.package))
        #expect(AppProfile.load(for: Self.package, in: sandbox.paths).installSources == nil)

        let granted = AppProfile(installSources: true)
        try granted.save(for: Self.package, in: sandbox.paths)

        #expect(AppProfile.load(for: Self.package, in: sandbox.paths).installSources == true)
        #expect(!granted.isDefault)
    }

    @Test func savingTheDefaultDeletesTheFile() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        try AppProfile(deviceProfile: .pixel).save(for: Self.package, in: sandbox.paths)

        try AppProfile().save(for: Self.package, in: sandbox.paths)

        #expect(!sandbox.exists(sandbox.paths.profileFile(for: Self.package)))
        // Nothing to delete is no error either.
        try AppProfile().save(for: Self.package, in: sandbox.paths)
    }

    /// A key a later version writes (MetalFX's, say) is passed over; a field that is not there is the default.
    @Test func unknownKeysAreIgnoredAndMissingOnesAreTheDefault() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        try write(#"{"frameRate": 60, "renderScale": 0.75}"#, in: sandbox)

        #expect(AppProfile.load(for: Self.package, in: sandbox.paths) == AppProfile(frameRate: .fps60))
    }

    /// A file that does not decode is the default, never an error, and the next save replaces it.
    @Test(arguments: [#"{"frameRate": 90}"#, #"{"deviceProfile": "oneplus"}"#, "not json", ""])
    func aFileThatDoesNotDecodeIsTheDefault(text: String) throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        try write(text, in: sandbox)

        #expect(AppProfile.load(for: Self.package, in: sandbox.paths) == AppProfile())

        try AppProfile(frameRate: .fps120).save(for: Self.package, in: sandbox.paths)
        #expect(AppProfile.load(for: Self.package, in: sandbox.paths) == AppProfile(frameRate: .fps120))
    }

    @Test func anUpdateKeepsTheProfile() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importFile(sandbox.file(SampleAPK.make(), named: "v1.apk"))
        try AppProfile(deviceProfile: .pixel, frameRate: .fps60).save(for: Self.package, in: sandbox.paths)

        _ = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        #expect(AppProfile.load(for: Self.package, in: sandbox.paths)
                == AppProfile(deviceProfile: .pixel, frameRate: .fps60))
    }

    @Test func removingTheAppRemovesItsProfile() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importFile(sandbox.file(SampleAPK.make(), named: "game.apk"))
        try AppProfile(frameRate: .fps120).save(for: Self.package, in: sandbox.paths)

        try await library.remove(id: Self.package)

        #expect(!sandbox.exists(sandbox.paths.profileFile(for: Self.package)))
    }

    private func write(_ text: String, in sandbox: Sandbox) throws {
        let file = sandbox.paths.profileFile(for: Self.package)
        try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data(text.utf8).write(to: file)
    }
}
