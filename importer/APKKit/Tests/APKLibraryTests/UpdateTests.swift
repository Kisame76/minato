import APKKit
import APKSigning
import APKTestSupport
import CryptoKit
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0034 step a): each version in a folder of its own, the switch through the index, what an update
/// keeps, and the files that change nothing.
@Suite("Library updates")
struct UpdateTests {
    typealias Sandbox = LibraryTests.Sandbox

    struct Interrupted: Error {}

    /// What lies in apps/<package>/, sorted.
    static func appFolder(_ sandbox: Sandbox, _ package: String = "org.example.game") -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: sandbox.paths.installDirectory(for: package).path)) ?? []).sorted()
    }

    /// The first 16 hex digits of the SHA-256, which name a version's folder.
    static func folder(of data: Data) -> String {
        String(SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined().prefix(16))
    }

    static func installedAPK(_ sandbox: Sandbox, _ entry: LibraryEntry) throws -> Data {
        try Data(contentsOf: sandbox.paths.versionDirectory(for: entry).appendingPathComponent("base.apk"))
    }

    // MARK: - Installing and updating

    @Test func installsIntoAFolderNamedAfterTheFile() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make()
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(sandbox.file(apk, named: "game.apk"))

        #expect(result.decision == .install)
        #expect(result.entry.folder == Self.folder(of: apk))
        #expect(Self.appFolder(sandbox) == [Self.folder(of: apk)])
        #expect(try Self.installedAPK(sandbox, result.entry) == apk)
        #expect(result.entry.iconFile == "apps/org.example.game/\(Self.folder(of: apk))/icon.png")
        #expect(result.entry.updatedAt == nil)
        #expect(result.report?.versionCode == 1)
    }

    @Test func anUpdateKeepsDataPlaceAndImportDateAndRemovesTheOldFolder() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")).entry
        _ = try await library.importFile(sandbox.file(SampleAPK.make(package: "org.example.other"), named: "other.apk"))
        let save = sandbox.paths.dataDirectory(for: first.id).appendingPathComponent("data/save.dat")
        try Data("progress".utf8).write(to: save)
        let before = Date()

        let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        #expect(result.decision == .update)
        #expect(result.entry.report?.versionCode == 2)
        #expect(result.entry.importedAt == first.importedAt)
        let updatedAt = try #require(result.entry.updatedAt)
        #expect(updatedAt >= before)
        #expect(await library.entries.map(\.id) == ["org.example.game", "org.example.other"])
        #expect(try Data(contentsOf: save) == Data("progress".utf8))
        let folder = Self.folder(of: SampleAPK.make(versionCode: 2))
        #expect(Self.appFolder(sandbox) == [folder])
        #expect(try Self.installedAPK(sandbox, result.entry) == SampleAPK.make(versionCode: 2))
        // What belongs to the version is the new one's.
        #expect(result.entry.base?.sha256.hasPrefix(folder) == true)
        #expect(result.entry.source.fileName == "v2.apk")
        #expect(result.entry.iconFile == "apps/org.example.game/\(folder)/icon.png")
        // After a restart the index names the new version.
        #expect(try await Library(paths: sandbox.paths).entries.first?.folder == result.entry.folder)
    }

    @Test func anUpdateStartsWithItsCodeUnsigned() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        // Apps without native code of their own, so that only the library written here counts.
        let first = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1, abis: []), named: "v1.apk")).entry
        let lib = sandbox.paths.libraryDirectory(for: first)
        try FileManager.default.createDirectory(at: lib, withIntermediateDirectories: true)
        try Data("elf".utf8).write(to: lib.appendingPathComponent("libmain.so"))
        try await library.resign(id: first.id, fingerprint: "aa", with: ResigningTests.RecordingConverter(fingerprint: "aa"))

        let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2, abis: []), named: "v2.apk"))

        // The old version's code went with its folder; the new version's is not converted or signed yet.
        #expect(result.entry.signedWith == nil)
        #expect(!sandbox.exists(lib))
        #expect(await library.resignCandidates() == [ResignCandidate(id: first.id, signedWith: nil, containerCount: 0)])
    }

    @Test func anUpdateTakesTheNewVersionsState() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1, splitRequired: true), named: "v1.apk")).entry
        #expect(first.state == .needsSplits)

        let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        #expect(result.decision == .update)
        #expect(result.entry.state == .ready)
    }

    @Test func anUpdateKeepsWhatItDoesNotReplace() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")).entry
        let obb = PackageFile(fileName: "main.1.org.example.game.obb", splitName: nil, size: 3, sha256: "ee")
        let split = PackageFile(fileName: "split_config.xxhdpi.apk", splitName: "config.xxhdpi", size: 4, sha256: "ff")
        try await library.record(id: first.id) {
            $0.obbs = [obb]
            $0.splits = [split]
        }

        let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        // OBBs belong to the entry, split APKs to the version (ADR-0034, decision 6).
        #expect(result.entry.id == first.id)
        #expect(result.entry.obbs == [obb])
        #expect(result.entry.splits.isEmpty)
    }

    @Test func theSameVersionWithOtherBytesInstallsAgain() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = try await library.importFile(sandbox.file(SampleAPK.make(label: "Before"), named: "a.apk")).entry
        let before = Date()

        let result = try await library.importFile(sandbox.file(SampleAPK.make(label: "After"), named: "b.apk"))

        #expect(result.decision == .reinstall)
        #expect(result.entry.displayName == "After")
        #expect(result.entry.importedAt == first.importedAt)
        let updatedAt = try #require(result.entry.updatedAt)
        #expect(updatedAt >= before)
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make(label: "After"))])
    }

    // MARK: - Questions (step b)

    /// A file the user picked, taken over into incoming/ as the app does.
    static func picked(_ sandbox: Sandbox, _ library: Library, _ data: Data, named name: String) async throws -> URL {
        try await library.adopt(sandbox.file(data, named: name))
    }

    static let olderVersion = SampleAPK.make(versionCode: 1)
    static let otherSigner = SampleAPK.make(versionCode: 3, signers: [.rotated])

    @Test(arguments: [(olderVersion, Set([UpdateWarning.olderVersion])), (otherSigner, Set([UpdateWarning.otherSigner]))])
    func aQuestionChangesNothingAndTheFileWaits(file: Data, warnings: Set<UpdateWarning>) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(Self.picked(sandbox, library, SampleAPK.make(versionCode: 2), named: "v2.apk")).entry
        let url = try await Self.picked(sandbox, library, file, named: "new.apk")

        let result = try await library.importFile(url)

        #expect(result.decision == .ask(warnings))
        #expect(result.entry == installed)
        #expect(result.report?.versionCode == (file == Self.olderVersion ? 1 : 3))
        #expect(await library.entries == [installed])
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make(versionCode: 2))])
        #expect(sandbox.exists(url))
        // Minato quit without an answer: the next launch finds the file again, and the question comes again.
        let relaunched = try Library(paths: sandbox.paths)
        #expect(LibraryTests.resolved(try await relaunched.scan()) == LibraryTests.resolved([url]))
        #expect(try await relaunched.importFile(url).decision == .ask(warnings))
        #expect(sandbox.exists(url))
    }

    @Test(arguments: [olderVersion, otherSigner])
    func replaceUpdatesAndKeepsTheData(file: Data) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(Self.picked(sandbox, library, SampleAPK.make(versionCode: 2), named: "v2.apk")).entry
        let save = sandbox.paths.dataDirectory(for: installed.id).appendingPathComponent("data/save.dat")
        try Data("progress".utf8).write(to: save)
        let url = try await Self.picked(sandbox, library, file, named: "new.apk")
        _ = try await library.importFile(url)

        let result = try await library.importFile(url, replacing: true)

        #expect(result.decision == .update)
        #expect(result.entry.report?.versionCode == (file == Self.olderVersion ? 1 : 3))
        #expect(result.entry.importedAt == installed.importedAt)
        #expect(result.entry.updatedAt != nil)
        #expect(try Self.installedAPK(sandbox, result.entry) == file)
        #expect(Self.appFolder(sandbox) == [Self.folder(of: file)])
        #expect(try Data(contentsOf: save) == Data("progress".utf8))
        #expect(!sandbox.exists(url))
        #expect(try await library.scan().isEmpty)
    }

    @Test(arguments: [olderVersion, otherSigner])
    func keepInstalledDiscardsTheFile(file: Data) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(Self.picked(sandbox, library, SampleAPK.make(versionCode: 2), named: "v2.apk")).entry
        let url = try await Self.picked(sandbox, library, file, named: "new.apk")
        _ = try await library.importFile(url)

        await library.keepInstalled(discarding: url)

        #expect(!sandbox.exists(url))
        #expect(try await library.scan().isEmpty)
        #expect(await library.entries == [installed])
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make(versionCode: 2))])
    }

    @Test func replacingChangesNothingWithoutAQuestion() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk")).entry

        let same = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "again.apk"), replacing: true)
        let unusable = try await library.importFile(
            sandbox.file(SampleAPK.make(versionCode: 3, abis: ["armeabi-v7a"], signers: [.rotated]), named: "v3.apk"),
            replacing: true)

        #expect(same.decision == .unchanged)
        #expect(unusable.decision == .reject(.incompatible))
        #expect(await library.entries == [installed])
    }

    @Test func anEntryFromBeforeTheSignerReadsItFromItsBaseAPK() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        // A rotated key: v2 carries the old key, v3 the new one with the old one in its lineage.
        let rotated = SampleAPK.make(versionCode: 1, v3: [TestSigner.Signer(.rotated, lineage: [.init(.ec), .init(.rotated)])])
        let imported = try await Library(paths: sandbox.paths).importFile(sandbox.file(rotated, named: "v1.apk")).entry
        #expect(imported.report?.signature.signer == SignerIdentity(
            current: [TestCertificates.rotatedSignerSHA256],
            lineage: [LineageNode(sha256: TestCertificates.signerSHA256, flags: TestSigner.defaultCapabilities)]))
        // Written before the signer: the report lists both certificates, the old and the new key, and no signer.
        var old = imported
        old.report?.signature.signer = nil
        old.report?.signature.check = nil
        old.report?.signature.proof = nil
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        let index = try encoder.encode(OldIndex(entries: [old]))
        #expect(!String(decoding: index, as: UTF8.self).contains("lineage"))
        try index.write(to: sandbox.paths.indexFile)
        let library = try Library(paths: sandbox.paths)

        // Only the new key now, without the lineage: its base.apk shows the installed signer is that key.
        let result = try await library.importFile(sandbox.file(
            SampleAPK.make(versionCode: 2, signers: [.rotated]), named: "v2.apk"))

        #expect(result.decision == .update)
    }

    // MARK: - The signature check (step d)

    /// A sample APK with the last byte of its icon changed after signing.
    static func damaged(versionCode: Int32) throws -> Data {
        var apk = SampleAPK.make(versionCode: versionCode)
        let archive = try ZipArchive(data: apk)
        let icon = try #require(archive.entry(named: "res/mipmap-xxhdpi-v4/ic_launcher.png"))
        apk[try Int(archive.dataOffset(of: icon) + icon.compressedSize) - 1] ^= 0x01
        return apk
    }

    static func signatureFailure(_ report: APKReport?) -> SignatureFailure? {
        for case .signatureInvalid(let failure) in report?.compatibility.issues ?? [] { return failure }
        return nil
    }

    @Test func aDamagedOrForgedUpdateIsRejected() async throws {
        let forged = SampleAPK.make(versionCode: 2) { TestSigner.sign($0, v2: [.init(.rotated, certificate: TestKey.ec.certificate)]) }
        for (file, kind) in [(try Self.damaged(versionCode: 2), SignatureFailure.Kind.damaged), (forged, .signatureMismatch)] {
            let sandbox = try Sandbox()
            defer { sandbox.remove() }
            let library = try Library(paths: sandbox.paths)
            let v1 = SampleAPK.make(versionCode: 1)
            let installed = try await library.importFile(sandbox.file(v1, named: "v1.apk")).entry

            let result = try await library.importFile(sandbox.file(file, named: "v2.apk"), replacing: true)

            #expect(result.decision == .reject(.incompatible))
            #expect(Self.signatureFailure(result.report)?.kind == kind)
            #expect(await library.entries == [installed])
            #expect(try Self.installedAPK(sandbox, installed) == v1)
        }
    }

    @Test func aDamagedFileIsRejectedOverAVersionThatCannotRunEither() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let v1 = SampleAPK.make(versionCode: 1, abis: ["armeabi-v7a"])
        let installed = try await library.importFile(sandbox.file(v1, named: "v1.apk")).entry

        let result = try await library.importFile(sandbox.file(Self.damaged(versionCode: 2), named: "v2.apk"), replacing: true)

        #expect(result.decision == .reject(.incompatible))
        #expect(await library.entries == [installed])
    }

    @Test func aDamagedFileInstallsAsIncompatible() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(Self.damaged(versionCode: 1), named: "v1.apk")).entry
        #expect(entry.state == .incompatible)
        #expect(Self.signatureFailure(entry.report)?.kind == .damaged)
        #expect(entry.report?.signature.check.map { if case .failed = $0 { true } else { false } } == true)
    }

    @Test func anUpdateTheCheckCannotProveAsks() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk"))
        // The same certificate, but only a verity signature, which the check cannot verify.
        let unproven = SampleAPK.make(versionCode: 2) { TestSigner.sign($0, v2: [.init(.ec, algorithms: [TestSigner.ecdsaSHA256Verity])]) }

        let result = try await library.importFile(sandbox.file(unproven, named: "v2.apk"))

        #expect(result.decision == .ask([.otherSigner]))
        #expect(result.report?.signature.signer == SignerIdentity(current: [TestKey.ec.sha256]))
    }

    @Test func anUnprovenInstalledVersionAsksBeforeTheRealOne() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        // Names the real signer's certificate, with a signature the check cannot verify: installed fresh.
        let unproven = SampleAPK.make(versionCode: 1) { TestSigner.sign($0, v2: [.init(.ec, algorithms: [TestSigner.ecdsaSHA256Verity])]) }
        #expect(try await library.importFile(sandbox.file(unproven, named: "v1.apk")).decision == .install)

        let real = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        #expect(real.decision == .ask([.otherSigner]))
    }

    @Test func anEntryFromBeforeTheCheckHasItsBaseAPKChecked() async throws {
        for (installedFile, decision) in [(SampleAPK.make(versionCode: 1), UpdateDecision.update),
                                          (SampleAPK.make(versionCode: 1) { TestSigner.sign($0, v2: [.init(.ec, algorithms: [TestSigner.ecdsaSHA256Verity])]) },
                                           .ask([.otherSigner]))] {
            let sandbox = try Sandbox()
            defer { sandbox.remove() }
            var entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(installedFile, named: "v1.apk")).entry
            entry.report?.signature.check = nil
            let encoder = JSONEncoder()
            encoder.dateEncodingStrategy = .iso8601
            try encoder.encode(OldIndex(entries: [entry])).write(to: sandbox.paths.indexFile)
            let library = try Library(paths: sandbox.paths)

            let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

            #expect(result.decision == decision)
        }
    }

    @Test func anEntryFromStepBReadsTheCapabilitiesFromItsBaseAPK() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        // Rotated to a new key that may roll back to the old one.
        let lineage: [TestSigner.Level] = [.init(.ec, flags: TestSigner.defaultCapabilities | TestSigner.rollback), .init(.rotated)]
        let rotated = SampleAPK.make(versionCode: 1, v3: [TestSigner.Signer(.rotated, lineage: lineage)])
        var entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(rotated, named: "v1.apk")).entry
        // Step b) recorded the lineage without the capabilities, and no check.
        entry.report?.signature.signer?.lineage = [LineageNode(sha256: TestKey.ec.sha256, flags: nil)]
        entry.report?.signature.check = nil
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        try encoder.encode(OldIndex(entries: [entry])).write(to: sandbox.paths.indexFile)
        let library = try Library(paths: sandbox.paths)

        // Back to the old key: the base.apk's lineage grants it.
        let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2, signers: [.ec]), named: "v2.apk"))

        #expect(result.decision == .update)
    }

    // MARK: - Files that change nothing

    /// A split APK of the sample app, signed with the same key, which holds its library.
    static func splitAPK(_ name: String, versionCode: Int32 = 1) -> Data {
        let manifest = AXMLBuilder.Element("manifest", [
            .plain("package", .string("org.example.game")),
            .plain("split", .string(name)),
            .android("versionCode", 0x0101_021B, .int(versionCode)),
        ], [AXMLBuilder.Element("application", [.android("hasCode", 0x0101_000C, .bool(false))])])
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", AXMLBuilder().build(manifest))
        zip.add("lib/arm64-v8a/libgame.so", ELFBuilder(segments: [.load(align: 0x4000)]).build())
        return TestSigner.sign(zip, v2: [TestSigner.Signer(.ec)])
    }

    @Test func aSplitPickedWithItsBaseNeverReplacesTheBase() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let base = SampleAPK.make(versionCode: 1, splitRequired: true)
        // "+" with both files: they wait in incoming/ and are imported in scan's order, as LibraryModel does.
        _ = try await library.adopt(sandbox.file(base, named: "base.apk"))
        _ = try await library.adopt(sandbox.file(Self.splitAPK("config.arm64_v8a"), named: "split_config.arm64_v8a.apk"))
        var decisions: [UpdateDecision] = []
        for file in try await library.scan() {
            decisions.append(try await library.importFile(file).decision)
        }

        #expect(decisions == [.install, .reject(.incompatible)])
        let entries = await library.entries
        #expect(entries.count == 1)
        let entry = try #require(entries.first)
        #expect(entry.state == .needsSplits)
        #expect(entry.base != nil)
        #expect(try Self.installedAPK(sandbox, entry) == base)
        #expect(try await library.scan().isEmpty)
    }

    @Test func aBaseImportedAfterItsSplitReplacesTheSplitsEntry() async throws {
        // The other order: the lone split's entry cannot run and keeps no APK, so its base replaces it.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let base = SampleAPK.make(versionCode: 1, splitRequired: true)
        _ = try await library.adopt(sandbox.file(Self.splitAPK("config.arm64_v8a"), named: "a_split.apk"))
        _ = try await library.adopt(sandbox.file(base, named: "base.apk"))
        var decisions: [UpdateDecision] = []
        for file in try await library.scan() {
            decisions.append(try await library.importFile(file).decision)
        }

        #expect(decisions == [.install, .reinstall])
        let entries = await library.entries
        #expect(entries.count == 1)
        let entry = try #require(entries.first)
        #expect(entry.state == .needsSplits)
        #expect(try Self.installedAPK(sandbox, entry) == base)
        #expect(try await library.scan().isEmpty)
    }

    @Test(arguments: [(["armeabi-v7a"], false, LibraryEntry.State.incompatible), (["arm64-v8a"], true, .needsSplits)])
    func anUnusableFileLeavesTheWorkingVersion(abis: [String], splitRequired: Bool, state: LibraryEntry.State) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")).entry
        let picked = try await library.adopt(
            sandbox.file(SampleAPK.make(versionCode: 2, abis: abis, splitRequired: splitRequired), named: "v2.apk"))
        let recorder = ProgressRecorder()

        let result = try await library.importFile(picked) { recorder.add($0) }

        let folder = try #require(installed.folder)
        #expect(recorder.values.last == 1)
        #expect(result.decision == .reject(state))
        #expect(result.report?.versionCode == 2)
        #expect(result.entry == installed)
        #expect(await library.entries == [installed])
        #expect(Self.appFolder(sandbox) == [folder])
        #expect(try Self.installedAPK(sandbox, installed) == SampleAPK.make(versionCode: 1))
        #expect(!sandbox.exists(picked))
    }

    @Test(arguments: [["arm64-v8a"], ["armeabi-v7a"]])
    func theSameBytesChangeNothing(abis: [String]) async throws {
        // Also for an incompatible version, which keeps no base.apk but the file's SHA-256.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make(abis: abis)
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(apk, named: "game.apk")).entry
        let picked = try await library.adopt(sandbox.file(apk, named: "again.apk"))
        let recorder = ProgressRecorder()

        let result = try await library.importFile(picked) { recorder.add($0) }

        let folder = try #require(installed.folder)
        #expect(recorder.values.last == 1)
        #expect(result.decision == .unchanged)
        #expect(result.entry == installed)
        #expect(await library.entries == [installed])
        #expect(Self.appFolder(sandbox) == [folder])
        #expect(!sandbox.exists(picked))
    }

    // MARK: - Interruptions

    @Test func anInterruptedUpdateLeavesTheOldVersionAndIsFinishedLater() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")).entry
        let picked = try await library.adopt(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))
        await library.interruptBeforeTheSwitch { throw Interrupted() }

        await #expect(throws: Interrupted.self) { try await library.importFile(picked) }

        // The new folder is complete, but the index still names the old one.
        let folder = try #require(installed.folder)
        let relaunched = try Library(paths: sandbox.paths)
        #expect(await relaunched.entries.first?.folder == folder)
        #expect(Self.appFolder(sandbox) == [folder, Self.folder(of: SampleAPK.make(versionCode: 2))].sorted())
        #expect(try Self.installedAPK(sandbox, installed) == SampleAPK.make(versionCode: 1))

        let waiting = try await relaunched.scan()

        #expect(Self.appFolder(sandbox) == [folder])
        #expect(LibraryTests.resolved(waiting) == LibraryTests.resolved([picked]))
        let result = try await relaunched.importFile(try #require(waiting.first))
        #expect(result.decision == .update)
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make(versionCode: 2))])
    }

    @Test func theSameFileAfterAnInterruptedImportBuildsItsFolderAgain() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk"))
        let picked = try await library.adopt(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))
        await library.interruptBeforeTheSwitch { throw Interrupted() }
        await #expect(throws: Interrupted.self) { try await library.importFile(picked) }
        await library.interruptBeforeTheSwitch {}

        // No scan in between: the complete folder of the interrupted import is still there.
        let result = try await library.importFile(picked)

        #expect(result.decision == .update)
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make(versionCode: 2))])
        #expect(try Self.installedAPK(sandbox, result.entry) == SampleAPK.make(versionCode: 2))
    }

    @Test func aVersionFolderInUseIsNeverCleared() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make()
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(apk, named: "game.apk")).entry
        let folder = try #require(installed.folder)
        // As if another file's SHA-256 began like the installed one's: the same folder, other bytes.
        try await library.record(id: installed.id) {
            $0.source.sha256 = String(repeating: "0", count: 64)
            $0.base?.sha256 = String(repeating: "0", count: 64)
        }
        let picked = try await library.adopt(sandbox.file(apk, named: "again.apk"))

        await #expect(throws: LibraryError.versionFolderInUse(folder)) { try await library.importFile(picked) }

        #expect(Self.appFolder(sandbox) == [folder])
        #expect(try Self.installedAPK(sandbox, installed) == apk)
    }

    @Test func aRecordThatCannotBeWrittenChangesNothing() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(SampleAPK.make(), named: "game.apk")).entry
        let lib = sandbox.paths.libraryDirectory(for: installed)
        try FileManager.default.createDirectory(at: lib, withIntermediateDirectories: true)
        try Data("elf".utf8).write(to: lib.appendingPathComponent("libmain.so"))
        try await library.resign(id: installed.id, fingerprint: "aa", with: ResigningTests.RecordingConverter(fingerprint: "aa"))
        // A folder stands where the index would be written.
        try FileManager.default.removeItem(at: sandbox.paths.indexFile)
        try FileManager.default.createDirectory(at: sandbox.paths.indexFile, withIntermediateDirectories: true)

        await #expect(throws: (any Error).self) {
            try await library.resign(id: installed.id, fingerprint: "bb", with: ResigningTests.RecordingConverter(fingerprint: "bb"))
        }
        await #expect(throws: (any Error).self) { try await library.forgetSignatures() }
        await #expect(throws: (any Error).self) { try await library.remove(id: installed.id) }

        // The library still holds what the index on disk says.
        #expect(await library.entries.map(\.id) == [installed.id])
        #expect(await library.entries.first?.signedWith == "aa")
    }

    @Test(arguments: [["arm64-v8a"], ["armeabi-v7a"]])
    func anIndexRewrittenByAnOlderBuildKeepsTheVersion(abis: [String]) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make(abis: abis)
        let installed = try await Library(paths: sandbox.paths).importFile(sandbox.file(apk, named: "game.apk")).entry
        let folder = try #require(installed.folder)
        // An older Minato build decodes the index and writes it again without the fields it does not know.
        let json = try #require(try JSONSerialization.jsonObject(with: Data(contentsOf: sandbox.paths.indexFile)) as? [String: Any])
        let entries = try #require(json["entries"] as? [[String: Any]])
        var old = json
        old["entries"] = entries.map { entry in
            var older = entry
            older["folder"] = nil
            older["updatedAt"] = nil
            if var source = older["source"] as? [String: Any] {
                source["sha256"] = nil
                older["source"] = source
            }
            // With a base.apk only its SHA-256 leads to the folder, without one only the icon's path.
            if older["base"] != nil { older["iconFile"] = nil }
            return older
        }
        try JSONSerialization.data(withJSONObject: old).write(to: sandbox.paths.indexFile)
        let written = try String(decoding: Data(contentsOf: sandbox.paths.indexFile), as: UTF8.self)
        #expect(!written.contains("folder"))

        // The folder is found again, by the base.apk's SHA-256 or else by the icon's path, so the
        // launch's scan keeps it.
        let library = try Library(paths: sandbox.paths)
        _ = try await library.scan()

        let entry = try #require(await library.entries.first)
        #expect(entry.folder == folder)
        #expect(Self.appFolder(sandbox) == [folder])
        #expect(sandbox.exists(sandbox.paths.versionDirectory(for: entry).appendingPathComponent("icon.png")))
    }

    @Test func anInterruptedFirstInstallLeavesNothingBehind() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let picked = try await library.adopt(sandbox.file(SampleAPK.make(), named: "game.apk"))
        await library.interruptBeforeTheSwitch { throw Interrupted() }
        await #expect(throws: Interrupted.self) { try await library.importFile(picked) }
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make())])

        let waiting = try await Library(paths: sandbox.paths).scan()

        #expect(!sandbox.exists(sandbox.paths.installDirectory(for: "org.example.game")))
        #expect(LibraryTests.resolved(waiting) == LibraryTests.resolved([picked]))
    }

    @Test func aSwitchThatCannotBeWrittenLeavesTheOldVersion() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk")).entry
        let picked = try await library.adopt(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))
        // A folder stands where the index would be written, so the switch fails.
        try FileManager.default.removeItem(at: sandbox.paths.indexFile)
        try FileManager.default.createDirectory(at: sandbox.paths.indexFile, withIntermediateDirectories: true)

        await #expect(throws: (any Error).self) { try await library.importFile(picked) }

        // The library still names the old version, so a scan removes the new folder, not the old one.
        let folder = try #require(installed.folder)
        #expect(await library.entries == [installed])
        _ = try await library.scan()
        #expect(Self.appFolder(sandbox) == [folder])
        #expect(try Self.installedAPK(sandbox, installed) == SampleAPK.make(versionCode: 1))
    }

    // MARK: - Entries from before version folders

    /// The index as the library wrote it before version folders: the entries without `folder` and `updatedAt`.
    struct OldIndex: Encodable {
        var version = 1
        var entries: [LibraryEntry]
    }

    @Test func anEntryFromBeforeVersionFoldersUpdatesIntoAFolder() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        // The layout before ADR-0034: base.apk, the icon and lib/ right in apps/<package>/. An app without native code
        // of its own, so that lib/ holds only the library written here.
        let imported = try await Library(paths: sandbox.paths)
            .importFile(sandbox.file(SampleAPK.make(versionCode: 1, abis: []), named: "v1.apk")).entry
        let app = sandbox.paths.installDirectory(for: imported.id)
        let version = sandbox.paths.versionDirectory(for: imported)
        for name in try FileManager.default.contentsOfDirectory(atPath: version.path) {
            try FileManager.default.moveItem(at: version.appendingPathComponent(name), to: app.appendingPathComponent(name))
        }
        try FileManager.default.removeItem(at: version)
        // From before signers.json too (ADR-0034 decision 2, as changed for ADR-0039 b3.5b1).
        try FileManager.default.removeItem(at: app.appendingPathComponent(SignersFile.fileName))
        try FileManager.default.createDirectory(at: app.appendingPathComponent("lib"), withIntermediateDirectories: true)
        try Data("elf".utf8).write(to: app.appendingPathComponent("lib/libold.so"))
        var old = imported
        old.folder = nil
        old.iconFile = "apps/\(imported.id)/icon.png"
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        let index = try encoder.encode(OldIndex(entries: [old]))
        #expect(!String(decoding: index, as: UTF8.self).contains("folder"))
        try index.write(to: sandbox.paths.indexFile)
        let save = sandbox.paths.dataDirectory(for: imported.id).appendingPathComponent("data/save.dat")
        try Data("progress".utf8).write(to: save)

        // Next to them, what an interrupted update left: a version folder, and names that are none.
        for name in ["0123456789abcdef", "notaversionfolde", "abc"] {
            try FileManager.default.createDirectory(at: app.appendingPathComponent(name), withIntermediateDirectories: true)
        }

        let library = try Library(paths: sandbox.paths)
        let entry = try #require(await library.entries.first)
        #expect(entry.folder == nil)
        #expect(sandbox.paths.versionDirectory(for: entry) == app)
        // A launch keeps the old layout's files, which belong to the entry, removes only the version folder, and
        // writes the signers.json the version lacks from its base.apk.
        #expect(try await library.scan().isEmpty)
        #expect(Self.appFolder(sandbox) == ["abc", "base.apk", "icon.png", "lib", "notaversionfolde", "signers.json"])
        #expect(await library.resignCandidates() == [ResignCandidate(id: entry.id, signedWith: nil, containerCount: 1)])

        let result = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 2), named: "v2.apk"))

        #expect(result.decision == .update)
        #expect(result.entry.importedAt == entry.importedAt)
        #expect(Self.appFolder(sandbox) == [Self.folder(of: SampleAPK.make(versionCode: 2))])
        #expect(try Data(contentsOf: save) == Data("progress".utf8))
    }
}
