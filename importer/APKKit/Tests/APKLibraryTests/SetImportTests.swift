import APKFormats
import APKKit
import APKTestSupport
import CryptoKit
import Foundation
import Testing

@testable import APKLibrary

/// A unit's split sets imported, stored and updated as one APK is (ADR-0048, decisions 2 to 6; step b5b1, tests 5 and 7).
@Suite("Set import")
struct SetImportTests {
    typealias Sandbox = LibraryTests.Sandbox

    static func sha256(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    /// A unit as `adopt` leaves one: `incoming/<uuid>/` with these files.
    static func unit(_ files: [String: Data], in paths: LibraryPaths) throws -> URL {
        let unit = paths.incoming.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: unit, withIntermediateDirectories: true)
        for (name, data) in files { try data.write(to: unit.appendingPathComponent(name)) }
        return unit
    }

    /// What the version folder of `entry` holds, by name.
    static func versionFiles(_ entry: LibraryEntry, _ paths: LibraryPaths) throws -> [String] {
        try FileManager.default.contentsOfDirectory(atPath: paths.versionDirectory(for: entry).path).sorted()
    }

    static let base = SplitAPK.make()
    static let arm64 = SplitAPK.make(split: "config.arm64_v8a", abis: ["arm64-v8a"])
    static let xxhdpi = SplitAPK.make(split: "config.xxhdpi")
    static let xxxhdpi = SplitAPK.make(split: "config.xxxhdpi")

    @Test func aSetIsStoredInAFolderNamedAfterItsDigest() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Self.unit(["base.apk": Self.base, "arm64.apk": Self.arm64, "xxhdpi.apk": Self.xxhdpi,
                                  "xxxhdpi.apk": Self.xxxhdpi], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        let entry = try #require(results.first?.entry)
        #expect(results.map(\.decision) == [.install])
        #expect(entry.id == SplitAPK.package && entry.state == .ready)
        let digest = try SetDigest.of([(nil, Self.sha256(Self.base)), ("config.arm64_v8a", Self.sha256(Self.arm64)),
                                       ("config.xxhdpi", Self.sha256(Self.xxhdpi))])
        #expect(entry.folder == String(digest.prefix(16)))
        #expect(entry.base == PackageFile(fileName: "base.apk", splitName: nil, size: UInt64(Self.base.count),
                                          sha256: Self.sha256(Self.base), kind: .base))
        #expect(entry.splits.map(\.fileName) == ["split_config.arm64_v8a.apk", "split_config.xxhdpi.apk"])
        #expect(entry.splits.map(\.kind) == [.config, .config])
        #expect(entry.skippedSplits?.map(\.splitName) == ["config.xxxhdpi"])
        #expect(entry.source.sha256 == Self.sha256(Self.base))
        let stored = try Self.versionFiles(entry, sandbox.paths)
        #expect(stored == ["base.apk", "signers.json", "split_config.arm64_v8a.apk", "split_config.xxhdpi.apk"])
        #expect(try Data(contentsOf: sandbox.paths.versionDirectory(for: entry).appendingPathComponent("split_config.xxhdpi.apk"))
            == Self.xxhdpi)
        #expect(!sandbox.exists(unit))
    }

    @Test func theSameSetAgainChangesNothing() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let files = ["base.apk": Self.base, "arm64.apk": Self.arm64]
        let first = try await library.importUnit(Self.unit(files, in: sandbox.paths))

        let again = try Self.unit(files, in: sandbox.paths)
        let results = try await library.importUnit(again)

        #expect(results.map(\.decision) == [.unchanged])
        #expect(results.first?.entry.folder == first.first?.entry.folder)
        #expect(!sandbox.exists(again))
    }

    @Test func anOlderIndexKeepsASetsFolder() async throws {
        // An older build that wrote the index again dropped `folder`; the set's digest finds it.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let installed = try #require(try await Library(paths: sandbox.paths).importUnit(
            Self.unit(["base.apk": Self.base, "arm64.apk": Self.arm64], in: sandbox.paths)).first?.entry)
        var index = try JSONSerialization.jsonObject(with: Data(contentsOf: sandbox.paths.indexFile)) as! [String: Any]
        var entries = index["entries"] as! [[String: Any]]
        entries[0]["folder"] = nil
        index["entries"] = entries
        try JSONSerialization.data(withJSONObject: index).write(to: sandbox.paths.indexFile)

        let relaunched = try Library(paths: sandbox.paths)
        _ = try await relaunched.scan()

        #expect(await relaunched.entries.first?.folder == installed.folder)
        let stored = try Self.versionFiles(installed, sandbox.paths)
        #expect(stored == ["base.apk", "signers.json", "split_config.arm64_v8a.apk"])
    }

    @Test func anInterruptedSetImportLeavesTheOldVersion() async throws {
        struct Interruption: Error {}
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let old = try #require(try await library.importUnit(
            Self.unit(["base.apk": Self.base, "arm64.apk": Self.arm64], in: sandbox.paths)).first?.entry)
        await library.interruptBeforeTheSwitch { throw Interruption() }

        let newer = try Self.unit(["base.apk": SplitAPK.make(versionCode: 2),
                                   "arm64.apk": SplitAPK.make(versionCode: 2, split: "config.arm64_v8a", abis: ["arm64-v8a"])],
                                  in: sandbox.paths)
        await #expect(throws: Interruption.self) { try await library.importUnit(newer) }

        #expect(await library.entries == [old])
        _ = try await Library(paths: sandbox.paths).scan()
        #expect(try FileManager.default.contentsOfDirectory(atPath: sandbox.paths.installDirectory(for: SplitAPK.package).path)
            == [old.folder!])
    }

    @Test func aSetWhoseSplitsDoNotFitThisIPhoneIsIncompatible() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let armv7 = SplitAPK.make(split: "config.armeabi_v7a", abis: ["armeabi-v7a"])

        let entry = try #require(try await library.importUnit(
            Self.unit(["base.apk": Self.base, "armv7.apk": armv7], in: sandbox.paths)).first?.entry)

        #expect(entry.state == .incompatible)
        #expect(entry.report?.compatibility.issues.contains(.splitChoice("No arm64-v8a split")) == true)
        #expect(entry.skippedSplits?.map(\.reason) == ["For the ABI armeabi-v7a; this iPhone runs arm64-v8a"])
        // Without a split installed, the folder keeps the base's SHA-256 (decision 6).
        #expect(entry.folder == String(Self.sha256(Self.base).prefix(16)))
    }

    @Test func theChoiceAloneCanMakeASetIncompatible() async throws {
        // Two device groups, neither this iPhone's and no fallback (decision 4).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Self.unit(["base.apk": Self.base, "a.apk": SplitAPK.make(split: "config.group_a"),
                                  "b.apk": SplitAPK.make(split: "config.group_b")], in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.state == .incompatible)
        #expect(entry.report?.compatibility.issues.count == 1)
        #expect(entry.report?.compatibility.issues.allSatisfy { if case .splitChoice = $0 { true } else { false } } == true)
    }

    @Test func eachSplitKeepsItsKind() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Self.unit(["base.apk": Self.base, "f.apk": SplitAPK.make(split: "dynamic", isFeatureSplit: true),
                                  "fc.apk": SplitAPK.make(split: "dynamic.config.xxhdpi", configForSplit: "dynamic"),
                                  "o.apk": SplitAPK.make(split: "extra")], in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.splits.map(\.splitName) == ["dynamic", "dynamic.config.xxhdpi", "extra"])
        #expect(entry.splits.map(\.kind) == [.feature, .config, .other])
        #expect(entry.splits.map(\.configForSplit) == [nil, "dynamic", nil])
    }

    @Test func aFreshSetMinatoCannotFullyCheckAsks() async throws {
        // A split signed by an algorithm Minato cannot verify, naming the base's certificate (step b3).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unverifiable = SplitAPK.make(split: "config.de", v2: [TestSigner.Signer(.dsa, certificate: TestKey.ec.certificate)])
        let unit = try Self.unit(["base.apk": Self.base, "de.apk": unverifiable], in: sandbox.paths)

        #expect(try await library.importUnit(unit).map(\.decision) == [.ask([.uncheckedContent])])
        #expect(await library.entries.isEmpty)
        #expect(sandbox.exists(unit))
        #expect(try await library.importUnit(unit, replacing: [AskingVersion(package: SplitAPK.package, versionCode: 1)]).map(\.decision) == [.install])
        #expect(await library.entries.map(\.id) == [SplitAPK.package])
    }

    @Test func aUnitsOtherFilesGoTheirOwnWay() async throws {
        // A file that is no APK is recorded as before; an OBB waits for ADR-0049.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Self.unit(["base.apk": Self.base, "arm64.apk": Self.arm64, "junk.apk": Data("junk".utf8),
                                  "main.1.org.example.sets.obb": Data("obb".utf8)], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(Set(results.map(\.entry.id)) == ["file:junk.apk", SplitAPK.package])
        #expect(try FileManager.default.contentsOfDirectory(atPath: unit.path) == ["main.1.org.example.sets.obb"])
    }

    @Test func aSetAndroidRefusesBecomesAnInvalidEntry() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Self.unit(["a.apk": Self.base, "b.apk": SplitAPK.make(files: ["assets/other": Data("x".utf8)])],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.map(\.entry.state) == [.invalid("Split base was defined multiple times")])
        #expect(results.map(\.entry.id) == ["file:a.apk"])
        #expect(!sandbox.exists(unit))
    }

    @Test func aLoneSplitIsRefusedWithoutItsBase() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let results = try await library.importUnit(Self.unit(["split_config.de.apk": SplitAPK.make(split: "config.de")],
                                                             in: sandbox.paths))

        #expect(results.map(\.entry.state)
            == [.invalid("Split APK “split_config.de.apk” needs its base APK. Import it together with its base APK.")])
    }

    @Test func aSetThatAsksWaitsUntilItIsAnswered() async throws {
        // The question covers the whole set (F7): an older version asks, "Replace" installs it.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(["base.apk": SplitAPK.make(versionCode: 2),
                                                    "arm64.apk": SplitAPK.make(versionCode: 2, split: "config.arm64_v8a",
                                                                               abis: ["arm64-v8a"])], in: sandbox.paths))
        let older = try Self.unit(["base.apk": Self.base, "arm64.apk": Self.arm64], in: sandbox.paths)

        #expect(try await library.importUnit(older).map(\.decision) == [.ask([.olderVersion])])
        #expect(sandbox.exists(older))
        #expect(try await library.importUnit(older, replacing: [AskingVersion(package: SplitAPK.package, versionCode: 1)]).map(\.decision) == [.update])
        #expect(await library.entries.first?.report?.versionCode == 1)
        #expect(!sandbox.exists(older))
    }

    @Test func aSetSignedOtherwiseAsks() async throws {
        // The set's signer comes from how the set is signed (step b3): APKs Minato cannot verify prove no signer.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(["base.apk": Self.base, "arm64.apk": Self.arm64], in: sandbox.paths))
        let unverifiable = try Self.unit(["base.apk": SplitAPK.make(versionCode: 2, signers: [.dsa]),
                                          "arm64.apk": SplitAPK.make(versionCode: 2, split: "config.arm64_v8a",
                                                                     abis: ["arm64-v8a"], signers: [.dsa])], in: sandbox.paths)

        #expect(try await library.importUnit(unverifiable).map(\.decision) == [.ask([.otherSigner])])
    }

    @Test func twoAppsInOneUnitAreTwoSets() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let other = "org.example.other"
        let unit = try Self.unit(["base.apk": Self.base, "arm64.apk": Self.arm64, "other.apk": SplitAPK.make(package: other),
                                  "other-arm64.apk": SplitAPK.make(package: other, split: "config.arm64_v8a", abis: ["arm64-v8a"])],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(Set(results.map(\.entry.id)) == [SplitAPK.package, other])
        #expect(results.allSatisfy { $0.entry.splits.map(\.splitName) == ["config.arm64_v8a"] })
    }

    @Test func oneAPKInAUnitIsImportedAsToday() async throws {
        // Its folder keeps its file's SHA-256 (decision 6), and a flat file is a unit of its own.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let flat = try sandbox.file(SampleAPK.make(), named: "game.apk", in: sandbox.paths.incoming)

        let entry = try #require(try await library.importUnit(flat).first?.entry)

        #expect(entry.id == "org.example.game" && entry.splits.isEmpty && entry.skippedSplits == nil)
        #expect(entry.folder == String(Self.sha256(SampleAPK.make()).prefix(16)))
        #expect(!sandbox.exists(flat))
    }
}
