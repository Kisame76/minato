import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// Splits without their base join the installed version, as Android's inherit mode adds splits to an installed app
/// (ADR-0048, decision 7; step b5b2, test 6).
@Suite("Lone splits")
struct LoneSplitTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Sets = SetImportTests

    static let de = SplitAPK.make(split: "config.de")

    static func needsItsBase(_ fileName: String) -> LibraryEntry.State {
        .invalid("Split APK “\(fileName)” needs its base APK. Import it together with its base APK.")
    }

    /// Installs a set of base and arm64 split, or `files`. The base's file is not named as in the version's folder, so
    /// the entry's source tells them apart.
    static func install(_ library: Library, _ paths: LibraryPaths,
                        _ files: [String: Data] = ["app.apk": Sets.base, "arm64.apk": Sets.arm64]) async throws -> LibraryEntry {
        try #require(try await library.importUnit(Sets.unit(files, in: paths)).first?.entry)
    }

    @Test func aSplitJoinsItsInstalledBase() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(sandbox.file(SampleAPK.make(), named: "game.apk", in: sandbox.paths.incoming))
        let old = try await Self.install(library, sandbox.paths)
        let unit = try Sets.unit(["de.apk": Self.de], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        let entry = try #require(results.first?.entry)
        #expect(results.map(\.decision) == [.reinstall])
        #expect(entry.splits.map(\.fileName) == ["split_config.arm64_v8a.apk", "split_config.de.apk"])
        let digest = try SetDigest.of([(nil, Sets.sha256(Sets.base)), ("config.arm64_v8a", Sets.sha256(Sets.arm64)),
                                       ("config.de", Sets.sha256(Self.de))])
        #expect(entry.folder == String(digest.prefix(16)))
        let stored = try Sets.versionFiles(entry, sandbox.paths)
        #expect(stored == ["base.apk", "lib", "signers.json", "split_config.arm64_v8a.apk", "split_config.de.apk"])
        #expect(entry.importedAt == old.importedAt && entry.source == old.source)
        // ADR-0034's switch: the old folder goes once the index names the new one.
        let folders = try FileManager.default.contentsOfDirectory(atPath: sandbox.paths.installDirectory(for: SplitAPK.package).path)
        #expect(folders == [entry.folder!])
        #expect(await library.entries.map(\.id) == ["org.example.game", SplitAPK.package])
        #expect(!sandbox.exists(unit))
    }

    @Test func severalSplitsJoinTogether() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox.paths)
        let unit = try Sets.unit(["f.apk": SplitAPK.make(split: "dynamic", isFeatureSplit: true),
                                  "fc.apk": SplitAPK.make(split: "dynamic.config.xxhdpi", configForSplit: "dynamic")],
                                 in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.splits.map(\.splitName) == ["config.arm64_v8a", "dynamic", "dynamic.config.xxhdpi"])
        #expect(entry.splits.map(\.kind) == [.config, .feature, .config])
    }

    @Test func aSplitOfTheSameNameReplacesTheInstalledOne() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let older = SplitAPK.make(split: "config.de", files: ["assets/text": Data("older".utf8)])
        _ = try await Self.install(library, sandbox.paths, ["base.apk": Sets.base, "arm64.apk": Sets.arm64, "de.apk": older])

        let entry = try #require(try await library.importUnit(Sets.unit(["de.apk": Self.de], in: sandbox.paths)).first?.entry)

        #expect(entry.splits.map(\.splitName) == ["config.arm64_v8a", "config.de"])
        #expect(entry.splits.last?.sha256 == Sets.sha256(Self.de))
        let stored = try Data(contentsOf: sandbox.paths.versionDirectory(for: entry).appendingPathComponent("split_config.de.apk"))
        #expect(stored == Self.de)
    }

    @Test func aSplitOfAnotherVersionCodeNeedsItsBase() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let old = try await Self.install(library, sandbox.paths)

        let results = try await library.importUnit(Sets.unit(["de.apk": SplitAPK.make(versionCode: 2, split: "config.de")],
                                                             in: sandbox.paths))

        #expect(results.map(\.entry.state) == [Self.needsItsBase("de.apk")])
        #expect(await library.entries.first { $0.id == SplitAPK.package } == old)
    }

    @Test func aSplitSignedOtherwiseNeedsItsBase() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let old = try await Self.install(library, sandbox.paths)

        let results = try await library.importUnit(Sets.unit(["de.apk": SplitAPK.make(split: "config.de", signers: [.rsa])],
                                                             in: sandbox.paths))

        #expect(results.map(\.entry.state) == [Self.needsItsBase("de.apk")])
        #expect(await library.entries.first { $0.id == SplitAPK.package } == old)
    }

    @Test func anInstalledVersionWithoutItsBaseTakesNoSplit() async throws {
        // An incompatible version keeps no APK (ADR-0034), and one whose APKs are not all there cannot be read.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let armv7 = SplitAPK.make(split: "config.armeabi_v7a", abis: ["armeabi-v7a"])
        let incompatible = try await Self.install(library, sandbox.paths, ["base.apk": Sets.base, "armv7.apk": armv7])
        #expect(incompatible.state == .incompatible && incompatible.base == nil)

        let results = try await library.importUnit(Sets.unit(["arm64.apk": Sets.arm64], in: sandbox.paths))
        #expect(results.map(\.entry.state) == [Self.needsItsBase("arm64.apk")])

        let installed = try await Self.install(library, sandbox.paths)
        let folder = sandbox.paths.versionDirectory(for: installed)
        try FileManager.default.removeItem(at: folder.appendingPathComponent("split_config.arm64_v8a.apk"))
        let unreadable = try await library.importUnit(Sets.unit(["de.apk": Self.de], in: sandbox.paths))
        #expect(unreadable.map(\.entry.state) == [Self.needsItsBase("de.apk")])
        #expect(await library.entries.first { $0.id == SplitAPK.package } == installed)
    }

    @Test func splitsCompleteABaseThatNeedsThem() async throws {
        // The joined set is checked again (checks 3 to 5): the installed base alone needs its splits.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let alone = try await Self.install(library, sandbox.paths, ["base.apk": SplitAPK.make(manifestSplitRequired: true)])
        #expect(alone.state == .needsSplits)

        let entry = try #require(try await library.importUnit(Sets.unit(["arm64.apk": Sets.arm64], in: sandbox.paths)).first?.entry)

        #expect(entry.state == .ready)
        #expect(entry.splits.map(\.splitName) == ["config.arm64_v8a"])
    }

    @Test func theJoinedSetIsChosenAgain() async throws {
        // The choice runs over the joined set (decision 4): a density split that fits better takes the installed
        // one's place.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox.paths, ["base.apk": Sets.base, "xhdpi.apk": SplitAPK.make(split: "config.xhdpi")])

        let entry = try #require(try await library.importUnit(Sets.unit(["xxhdpi.apk": Sets.xxhdpi], in: sandbox.paths)).first?.entry)

        #expect(entry.splits.map(\.splitName) == ["config.xxhdpi"])
        #expect(entry.skippedSplits?.map(\.fileName) == ["split_config.xhdpi.apk"])
    }

    @Test func aSplitTheChoiceSkipsChangesNothing() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let old = try await Self.install(library, sandbox.paths)
        let unit = try Sets.unit(["x86.apk": SplitAPK.make(split: "config.x86_64", abis: ["x86_64"])], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.map(\.decision) == [.unchanged])
        #expect(await library.entries == [old])
        #expect(!sandbox.exists(unit))
    }

    @Test func aSplitThatWouldBreakAWorkingVersionIsTurnedAway() async throws {
        // F2's policy row stays the last guard (decision 7): an arm64 split with other code makes the set incompatible.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let old = try await Self.install(library, sandbox.paths)
        var elf = ELFBuilder(segments: [.load(align: 0x4000), .load(align: 0x4000), .dynamic])
        elf.machine = 62
        let other = SplitAPK.make(split: "config.arm64_v8a", files: ["lib/arm64-v8a/libmain.so": elf.build()])

        let results = try await library.importUnit(Sets.unit(["arm64.apk": other], in: sandbox.paths))

        #expect(results.map(\.decision) == [.reject(.incompatible)])
        #expect(await library.entries == [old])
    }

    @Test func aJoinedSetAndroidRefusesBecomesAnInvalidEntry() async throws {
        // Split names that differ only in case (decision 3), here once the split joins the installed one.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let old = try await Self.install(library, sandbox.paths, ["base.apk": Sets.base, "de.apk": Self.de])

        let results = try await library.importUnit(Sets.unit(["De.apk": SplitAPK.make(split: "config.De")], in: sandbox.paths))

        #expect(results.map(\.entry.state) == [.invalid("Splits config.De and config.de differ only in case")])
        #expect(results.map(\.entry.id) == ["file:De.apk"])
        #expect(await library.entries.first { $0.id == SplitAPK.package } == old)
    }

    @Test func aJoinedSetThatAsksWaitsUntilItIsAnswered() async throws {
        // A split Minato cannot verify, naming the base's certificate: the joined set asks (step b3), and the unit
        // stays.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let old = try await Self.install(library, sandbox.paths)
        let unverifiable = SplitAPK.make(split: "config.de", v2: [TestSigner.Signer(.dsa, certificate: TestKey.ec.certificate)])
        let unit = try Sets.unit(["de.apk": unverifiable], in: sandbox.paths)

        #expect(try await library.importUnit(unit).map(\.decision) == [.ask([.uncheckedContent])])
        #expect(await library.entries == [old])
        #expect(sandbox.exists(unit))
        let shown = AskingVersion(package: SplitAPK.package, versionCode: 1, warnings: [.uncheckedContent])
        #expect(try await library.importUnit(unit, replacing: [shown]).map(\.decision) == [.update])
        #expect(await library.entries.first?.splits.map(\.splitName) == ["config.arm64_v8a", "config.de"])
    }

    // MARK: - Signers, schemes and changed bytes

    @Test func theFirstSplitNeedsTheSchemeTheBasesTargetSdkAsks() async throws {
        // Android's session takes its signer from its first APK, here a split, and holds it to the scheme of the
        // package's targetSdkVersion (ScanPackageUtils.java:740-751): v2 or newer from 30 on.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox.paths, ["app.apk": SplitAPK.make(targetSdk: 34), "arm64.apk": Sets.arm64])

        let refused = try await library.importUnit(Sets.unit(["de.apk": SplitAPK.make(split: "config.de", v1Only: true)],
                                                             in: sandbox.paths))
        #expect(refused.map(\.entry.state)
            == [.invalid("No signature found in package of version 2 or newer for package \(SplitAPK.package)")])

        // Minato's first is the first split by name, as SetChecks orders them: config.de, though a.apk's config.en comes
        // first by file name. The others need only its signer.
        let unit = try Sets.unit(["de.apk": Self.de, "a.apk": SplitAPK.make(split: "config.en", v1Only: true)], in: sandbox.paths)
        #expect(try await library.importUnit(unit).map(\.decision) == [.reinstall])
    }

    @Test func aV1OnlySplitJoinsABaseThatTargetsBelow30() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox.paths, ["app.apk": SplitAPK.make(targetSdk: 29), "arm64.apk": Sets.arm64])

        let results = try await library.importUnit(Sets.unit(["de.apk": SplitAPK.make(split: "config.de", v1Only: true)],
                                                             in: sandbox.paths))

        #expect(results.map(\.decision) == [.reinstall])
    }

    @Test func aJoinThatChangesNothingDoesNotAsk() async throws {
        // An unsigned install proves no signer, so such a join asked, and "Replace" then threw versionFolderInUse
        // out of importUnit at every scan: the same digest changes nothing first.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let old = try await Self.install(library, sandbox.paths, [
            "app.apk": SplitAPK.make(signers: []),
            "arm64.apk": SplitAPK.make(split: "config.arm64_v8a", abis: ["arm64-v8a"], signers: []),
        ])
        let unit = try Sets.unit(["x86.apk": SplitAPK.make(split: "config.x86_64", abis: ["x86_64"], signers: [])],
                                 in: sandbox.paths)

        #expect(try await library.importUnit(unit).map(\.decision) == [.unchanged])
        #expect(await library.entries == [old])
        #expect(!sandbox.exists(unit))

        // A split that changes something still asks, as the install proves no signer.
        let de = try Sets.unit(["de.apk": SplitAPK.make(split: "config.de", signers: [])], in: sandbox.paths)
        #expect(try await library.importUnit(de).map(\.decision) == [.ask([.otherSigner])])
    }

    @Test func anInstalledAPKWhoseBytesChangedTakesNoSplit() async throws {
        // The installed APKs are hashed again: bytes other than library.json names are no version to join.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await Self.install(library, sandbox.paths)
        let split = sandbox.paths.versionDirectory(for: installed).appendingPathComponent("split_config.arm64_v8a.apk")
        // In place of the read-only file, as a rename puts it there.
        try SplitAPK.make(split: "config.arm64_v8a", abis: ["arm64-v8a"], files: ["assets/other": Data("x".utf8)])
            .write(to: split, options: .atomic)

        let results = try await library.importUnit(Sets.unit(["de.apk": Self.de], in: sandbox.paths))

        #expect(results.map(\.entry.state) == [Self.needsItsBase("de.apk")])
    }

    @Test func anInstalledAPKOfAnotherSignerTakesNoSplit() async throws {
        // Android takes the installed APKs unverified (PackageInstallerSession.java:3680-3681, 3709); Minato checks
        // them as a set again, and one that no longer passes is no version to join, even where library.json names its
        // bytes.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let installed = try await Self.install(try Library(paths: sandbox.paths), sandbox.paths)
        let other = SplitAPK.make(split: "config.arm64_v8a", abis: ["arm64-v8a"], signers: [.rsa])
        try other.write(to: sandbox.paths.versionDirectory(for: installed).appendingPathComponent("split_config.arm64_v8a.apk"),
                        options: .atomic)
        var index = try JSONSerialization.jsonObject(with: Data(contentsOf: sandbox.paths.indexFile)) as! [String: Any]
        var entries = index["entries"] as! [[String: Any]]
        var splits = entries[0]["splits"] as! [[String: Any]]
        splits[0]["sha256"] = Sets.sha256(other)
        splits[0]["size"] = other.count
        entries[0]["splits"] = splits
        index["entries"] = entries
        try JSONSerialization.data(withJSONObject: index).write(to: sandbox.paths.indexFile)

        let results = try await Library(paths: sandbox.paths).importUnit(Sets.unit(["de.apk": Self.de], in: sandbox.paths))

        #expect(results.map(\.entry.state) == [Self.needsItsBase("de.apk")])
    }

    @Test func aSplitMinatoCannotVerifyNamingAnotherCertificateNeedsItsBase() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.install(library, sandbox.paths)
        let other = SplitAPK.make(split: "config.de", v2: [TestSigner.Signer(.dsa, certificate: TestKey.rsa.certificate)])

        let results = try await library.importUnit(Sets.unit(["de.apk": other], in: sandbox.paths))

        #expect(results.map(\.entry.state) == [Self.needsItsBase("de.apk")])
    }
}
