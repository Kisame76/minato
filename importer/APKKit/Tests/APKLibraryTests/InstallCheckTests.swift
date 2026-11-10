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

    /// Why, as Android's installer says it (APKRT_EVENT_INSTALL_RESULT's a2, which the framework turns into
    /// INSTALL_FAILED_* and PackageInstaller's STATUS_FAILURE_*): a file that is no valid app is invalid, one the
    /// iPhone cannot run (32-bit only) incompatible.
    @Test func aRefusalSaysWhyAsAndroidsInstallerDoes() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let cases: [(Data, String, InstallFailureReason)] = [
            (SampleAPK.make(abis: ["armeabi-v7a"]), "old.apk", .incompatible),
            (SampleAPK.make(splitRequired: true), "base.apk", .invalid),
            (Data("not a zip".utf8), "junk.apk", .invalid),
            (SampleAPK.make(signers: []), "unsigned.apk", .invalid),
        ]
        for (bytes, name, reason) in cases {
            let check = await library.checkForInstall(try sandbox.file(bytes, named: name))
            #expect(check.refusal == reason, "\(name): \(check)")
        }
        let installed = SampleAPK.make(versionCode: 2)
        _ = try await library.importFile(sandbox.file(installed, named: "v2.apk"))
        let older = try sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")
        let otherSigner = try sandbox.file(SampleAPK.make(versionCode: 3, signers: [.rsa]), named: "other.apk")
        let both = try sandbox.file(SampleAPK.make(versionCode: 1, signers: [.rsa]), named: "both.apk")
        #expect(await library.checkForInstall(older).refusal == .downgrade)
        #expect(await library.checkForInstall(otherSigner).refusal == .conflict)
        // Another signer goes first, as the framework's own check of the app's own package orders them.
        #expect(await library.checkForInstall(both).refusal == .conflict)
        #expect(await library.checkForInstall(try sandbox.file(installed, named: "same.apk")).refusal == nil)
        #expect(InstallFailureReason.none.rawValue == 0 && InstallFailureReason.invalid.rawValue == 1
                && InstallFailureReason.conflict.rawValue == 2 && InstallFailureReason.downgrade.rawValue == 3
                && InstallFailureReason.storage.rawValue == 4 && InstallFailureReason.incompatible.rawValue == 5)
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

    /// The import's own checks of the native code (ADR-0055): a library of the start path that cannot load refuses the
    /// app, so the check refuses it too, with the library's reason; nothing stays behind.
    @Test func aLibraryTheImportWouldRefuseIsRefused() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: NativeCodeFindingsTests.Checker(reasons: ["libgame.so": ["no arm64 code"]]))
        let file = try sandbox.file(SampleAPK.make(nativeActivity: true), named: "game.apk")

        guard case .refused(let why, .incompatible) = await library.checkForInstall(file) else {
            Issue.record("a refused library was accepted")
            return
        }
        #expect(why.contains("libgame.so"))
        #expect(await library.entries.isEmpty)
    }

    /// An APK that waits in incoming/ for the library's next start counts as installed: after version 2 was handed
    /// over, version 1 is a downgrade, as on Android once version 2 is in.
    @Test func aWaitingImportCountsAsInstalled() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let two = SampleAPK.make(versionCode: 2)
        _ = try await library.adopt(sandbox.file(two, named: "v2.apk"))

        let older = try sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")
        let same = try sandbox.file(two, named: "again.apk")
        guard case .refused = await library.checkForInstall(older) else { Issue.record("older accepted"); return }
        #expect(await library.checkForInstall(same) == .acceptable(package: "org.example.game", label: "Sample Game"))
    }

    /// A set an installer session holds (base.apk and split_*.apk, as SAI hands them over): checked as the import takes a
    /// unit of them, its base with the splits Minato would choose (ADR-0048); nothing in the library changes.
    @Test func aSplitSetIsCheckedAsTheImportWouldTakeIt() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let base = try sandbox.file(SplitAPK.make(manifestSplitRequired: true), named: "base.apk")
        let arm64 = try sandbox.file(SetImportTests.arm64, named: "split_config.arm64_v8a.apk")
        let xxhdpi = try sandbox.file(SetImportTests.xxhdpi, named: "split_config.xxhdpi.apk")

        guard case .acceptable(package: SplitAPK.package, _) = await library.checkForInstall([base, arm64, xxhdpi]) else {
            Issue.record("the set was refused")
            return
        }
        // The base alone needs its splits; one file is the plain APK's check.
        #expect(await library.checkForInstall([base]) == .refused("needs split APKs", .invalid))
        #expect(await library.entries.isEmpty)
    }

    @Test func aSetWithoutItsBaseOrWithAnotherAppsSplitIsRefused() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let base = try sandbox.file(SetImportTests.base, named: "base.apk")
        let arm64 = try sandbox.file(SetImportTests.arm64, named: "split_config.arm64_v8a.apk")
        let foreign = try sandbox.file(SplitAPK.make(package: "org.example.other", split: "config.xxhdpi"),
                                       named: "split_config.xxhdpi.apk")

        for set in [[arm64], [base, foreign], [base, arm64, arm64]] {
            guard case .refused = await library.checkForInstall(set) else {
                Issue.record("accepted: \(set.map(\.lastPathComponent))")
                continue
            }
        }
    }
}
