import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// An app installer's handed-over APK (APKRT_REQUEST_CONFIRM_INSTALL): Android's package installer answers RESULT_OK
/// only once the app is installed, so the library checks the file before Minato answers, and a file the import would
/// turn away is refused now. The check changes nothing in the library.
@Suite("Install check")
struct InstallCheckTests {
    typealias Sandbox = LibraryTests.Sandbox

    @Test func aCompatibleAPKIsAcceptedAndNothingChanges() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let file = try sandbox.file(SampleAPK.make(label: "Sample Game"), named: "game.apk")

        #expect(await library.checkForInstall(file) == .acceptable(package: "org.example.game", label: "Sample Game"))
        #expect(await library.entries.isEmpty)
        #expect(FileManager.default.fileExists(atPath: file.path))
    }

    @Test func aFileTheImportWouldNotRunIsRefused() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let thirtyTwoBit = try sandbox.file(SampleAPK.make(abis: ["armeabi-v7a"]), named: "old.apk")
        let needsSplits = try sandbox.file(SampleAPK.make(splitRequired: true), named: "base.apk")
        let notAnAPK = try sandbox.file(Data("not a zip".utf8), named: "junk.apk")
        let unsigned = try sandbox.file(SampleAPK.make(signers: []), named: "unsigned.apk")

        for file in [thirtyTwoBit, needsSplits, notAnAPK, unsigned] {
            guard case .refused = await library.checkForInstall(file) else {
                Issue.record("\(file.lastPathComponent) was accepted")
                continue
            }
        }
        #expect(await library.entries.isEmpty)
    }

    @Test func anUpdateIsCheckedAsAndroidInstallsIt() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = SampleAPK.make(versionCode: 2)
        _ = try await library.importFile(sandbox.file(installed, named: "v2.apk"))

        // The same bytes and a newer version install; an older one and another signer fail on Android
        // (INSTALL_FAILED_VERSION_DOWNGRADE, INSTALL_FAILED_UPDATE_INCOMPATIBLE), where the library would only ask.
        let same = try sandbox.file(installed, named: "same.apk")
        let newer = try sandbox.file(SampleAPK.make(versionCode: 3), named: "v3.apk")
        let older = try sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")
        let otherSigner = try sandbox.file(SampleAPK.make(versionCode: 3, signers: [.rsa]), named: "other.apk")

        #expect(await library.checkForInstall(same) == .acceptable(package: "org.example.game", label: "Sample Game"))
        #expect(await library.checkForInstall(newer) == .acceptable(package: "org.example.game", label: "Sample Game"))
        guard case .refused = await library.checkForInstall(older) else { Issue.record("older accepted"); return }
        guard case .refused = await library.checkForInstall(otherSigner) else { Issue.record("other signer"); return }
        #expect(await library.entries.count == 1)
    }
}
