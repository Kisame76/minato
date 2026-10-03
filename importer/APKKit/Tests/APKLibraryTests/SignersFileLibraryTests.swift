import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0034 decision 2 as changed for ADR-0039 b3.5b1: each version's folder keeps signers.json, which the guest's
/// PackageManager answers from: what the signature check proved, so only a verified version has it. A scan makes the
/// file what the report's proof says, and a version whose report has no proof yet gets its base.apk checked once.
@Suite("Library signers.json")
struct SignersFileLibraryTests {
    typealias Sandbox = LibraryTests.Sandbox

    static func signersFile(_ sandbox: Sandbox, _ entry: LibraryEntry) -> URL {
        sandbox.paths.versionDirectory(for: entry).appendingPathComponent(SignersFile.fileName)
    }

    /// The file for what the check of `apk` proves, as the analyzer checks it.
    static func expected(_ apk: Data) throws -> Data {
        let proof = try #require(try APKAnalyzer.analyze(ZipArchive(data: apk), preferredLanguages: ["en"]).signature.proof)
        return try SignersFile.content(of: proof)
    }

    /// Signed with a verity algorithm only: Android checks it, Minato cannot.
    static func unverified() -> Data {
        SampleAPK.make(sign: { TestSigner.sign($0, v2: [.init(.ec, algorithms: [TestSigner.ecdsaSHA256Verity])]) })
    }

    @Test func anImportWritesTheVersionsSignersFile() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make()
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(apk, named: "game.apk")).entry

        let written = try Data(contentsOf: Self.signersFile(sandbox, entry))
        #expect(written == (try Self.expected(apk)))
        #expect(entry.report?.signature.proof == SignerProof(scheme: .v2, certificates: [TestKey.ec.certificate]))
    }

    @Test func anUnsignedAPKGetsNone() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths)
            .importFile(sandbox.file(SampleAPK.make(signers: []), named: "game.apk")).entry

        #expect(entry.base != nil)
        #expect(!sandbox.exists(Self.signersFile(sandbox, entry)))
    }

    @Test func anUnverifiedAPKGetsNone() async throws {
        // Installed, but the guest sees it unsigned: the file holds only what was proven.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(Self.unverified(), named: "game.apk")).entry

        guard case .unverified = entry.report?.signature.check else { Issue.record("\(String(describing: entry.report?.signature.check))"); return }
        #expect(entry.base != nil)
        #expect(!sandbox.exists(Self.signersFile(sandbox, entry)))
    }

    @Test func aVersionWithoutBaseAPKGetsNone() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths)
            .importFile(sandbox.file(SampleAPK.make(abis: ["armeabi-v7a"]), named: "game.apk")).entry

        #expect(entry.base == nil)
        #expect(!sandbox.exists(Self.signersFile(sandbox, entry)))
    }

    @Test func v1AloneFromTargetSdk30IsNotInstalled() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make(targetSdk: 30, sign: { TestSigner.sign($0, v1: TestSigner.V1([.ec])) })
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(apk, named: "game.apk")).entry

        #expect(entry.state == .incompatible)
        guard case .failed(let failure) = entry.report?.signature.check else { Issue.record("not failed"); return }
        #expect(failure.kind == .noV2Signature)
        #expect(entry.base == nil)
    }

    @Test func anUpdateBringsItsOwnWithTheLineage() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importFile(sandbox.file(SampleAPK.make(versionCode: 1), named: "v1.apk"))
        // The key rotated: v3 names the new key with the old one in its lineage.
        let rotated = SampleAPK.make(versionCode: 2, v3: [TestSigner.Signer(.rotated, lineage: [.init(.ec), .init(.rotated)])])

        let entry = try await library.importFile(sandbox.file(rotated, named: "v2.apk")).entry

        let written = try Data(contentsOf: Self.signersFile(sandbox, entry))
        #expect(written == (try Self.expected(rotated)))
        #expect(String(decoding: written, as: UTF8.self).contains(#""scheme":3"#))
        #expect(UpdateTests.appFolder(sandbox) == [UpdateTests.folder(of: rotated)])
    }

    @Test func aScanWritesAMissingFile() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make()
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(apk, named: "game.apk")).entry
        try FileManager.default.removeItem(at: Self.signersFile(sandbox, entry))

        _ = try await Library(paths: sandbox.paths).scan()

        #expect(try Data(contentsOf: Self.signersFile(sandbox, entry)) == (try Self.expected(apk)))
    }

    @Test func aScanRewritesAFileThatIsNotTheProof() async throws {
        // Garbage would stop every start of the app; another signer would be a lie.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make()
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(apk, named: "game.apk")).entry
        for wrong in [Data("kept".utf8), try Self.expected(SampleAPK.make(signers: [.rotated]))] {
            try wrong.write(to: Self.signersFile(sandbox, entry))

            _ = try await Library(paths: sandbox.paths).scan()

            #expect(try Data(contentsOf: Self.signersFile(sandbox, entry)) == (try Self.expected(apk)))
        }
    }

    @Test func aScanRemovesTheFileOfAVersionNotProven() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(Self.unverified(), named: "game.apk")).entry
        try Self.expected(SampleAPK.make()).write(to: Self.signersFile(sandbox, entry))

        _ = try await Library(paths: sandbox.paths).scan()

        #expect(!sandbox.exists(Self.signersFile(sandbox, entry)))
    }

    @Test(arguments: [true, false])
    func aScanChecksAVersionWhoseReportHasNoProofOnce(checked: Bool) async throws {
        // A report from before the proof was recorded: verified, or from before the check.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let apk = SampleAPK.make()
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(apk, named: "game.apk")).entry
        try await Library(paths: sandbox.paths).record(id: entry.id) {
            $0.report?.signature.proof = nil
            if !checked { $0.report?.signature.check = nil }
        }
        try FileManager.default.removeItem(at: Self.signersFile(sandbox, entry))

        _ = try await Library(paths: sandbox.paths).scan()

        #expect(try Data(contentsOf: Self.signersFile(sandbox, entry)) == (try Self.expected(apk)))
        let kept = try #require(try Library.entries(in: sandbox.paths).first)
        #expect(kept.report?.signature.check == .verified)
        #expect(kept.report?.signature.proof == SignerProof(scheme: .v2, certificates: [TestKey.ec.certificate]))
        // Once: the next scan takes the kept proof, without reading base.apk.
        let base = sandbox.paths.versionDirectory(for: entry).appendingPathComponent("base.apk")
        try Data("gone".utf8).write(to: base)
        try FileManager.default.removeItem(at: Self.signersFile(sandbox, entry))
        _ = try await Library(paths: sandbox.paths).scan()
        #expect(try Data(contentsOf: Self.signersFile(sandbox, entry)) == (try Self.expected(apk)))
    }

    @Test func aScanLeavesAReportItCannotProveWithoutAFile() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths).importFile(sandbox.file(Self.unverified(), named: "game.apk")).entry
        try await Library(paths: sandbox.paths).record(id: entry.id) { $0.report?.signature.check = nil }

        _ = try await Library(paths: sandbox.paths).scan()

        #expect(!sandbox.exists(Self.signersFile(sandbox, entry)))
        guard case .unverified = try #require(try Library.entries(in: sandbox.paths).first).report?.signature.check else {
            Issue.record("the check was not kept")
            return
        }
    }
}
