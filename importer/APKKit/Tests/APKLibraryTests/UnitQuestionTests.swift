import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Synchronization
import Testing

@testable import APKLibrary

/// The library side of the app's units (ADR-0048 b7a and b7b, F7): a scan by unit, a question that answers only the sets
/// it listed as it listed them, the files Minato cannot check, the text of a set Android refuses, "Keep Installed" for a
/// unit, and a unit's progress.
@Suite("Questions per unit")
struct UnitQuestionTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Sets = SetImportTests

    @Test func aScanGivesEachWaitingPickOnceOldestFirst() async throws {
        // A flat file of an older build is a unit of its own; a pick still being taken over waits; one of OBBs only is a
        // unit since ADR-0049 d.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let flat = try sandbox.file(SampleAPK.make(), named: "flat.apk", in: sandbox.paths.incoming)
        let library = try Library(paths: sandbox.paths)
        let unit = try await library.adopt([try sandbox.file(SampleAPK.make(package: "org.example.one"), named: "one.apk"),
                                            try sandbox.file(SampleAPK.make(package: "org.example.two"), named: "two.apk")])
        let partial = sandbox.paths.incoming.appendingPathComponent(UUID().uuidString + ".partial", isDirectory: true)
        _ = try sandbox.file(SampleAPK.make(), named: "game.apk", in: partial)
        let obbOnly = sandbox.paths.incoming.appendingPathComponent(UUID().uuidString, isDirectory: true)
        _ = try sandbox.file(Data("obb".utf8), named: "main.1.org.example.game.obb", in: obbOnly)

        #expect(LibraryTests.resolved(try await library.scanUnits()) == LibraryTests.resolved([flat, unit, obbOnly]))
        #expect(LibraryTests.resolved(try await library.scan())
            == LibraryTests.resolved([flat, unit.appendingPathComponent("one.apk"), unit.appendingPathComponent("two.apk")]))
    }

    /// A unit with version 1 of a set and of one APK, over their installed version 2: both ask about the older version.
    static func olderUnit(_ library: Library, _ paths: LibraryPaths) async throws -> URL {
        _ = try await library.importUnit(Sets.unit(["base.apk": SplitAPK.make(versionCode: 2),
                                                    "arm64.apk": SplitAPK.make(versionCode: 2, split: "config.arm64_v8a",
                                                                               abis: ["arm64-v8a"])], in: paths))
        _ = try await library.importUnit(Sets.unit(["b.apk": SplitAPK.make(package: "org.example.b", versionCode: 2)], in: paths))
        return try Sets.unit(["base.apk": Sets.base, "arm64.apk": Sets.arm64, "b.apk": SplitAPK.make(package: "org.example.b")],
                             in: paths)
    }

    @Test(arguments: [true, false])
    func replaceAnswersOnlyTheVersionsTheQuestionListed(setFirst: Bool) async throws {
        // Replace for one asking set used to answer every set of the unit (the review, b5b2). The set goes
        // through importSet and the APK through importFile, so each is answered first once.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try await Self.olderUnit(library, sandbox.paths)
        let set = AskingVersion(package: SplitAPK.package, versionCode: 1, warnings: [.olderVersion])
        let apk = AskingVersion(package: "org.example.b", versionCode: 1, warnings: [.olderVersion])
        // The set's group comes first: it has the unit's first file, arm64.apk.
        #expect(try await library.importUnit(unit).map(\.asking) == [set, apk])

        let first = try await library.importUnit(unit, replacing: [setFirst ? set : apk])
        #expect(first.map(\.decision) == (setFirst ? [.update, .ask([.olderVersion])] : [.ask([.olderVersion]), .update]))
        #expect(first.map(\.asking) == (setFirst ? [nil, apk] : [set, nil]))
        #expect(sandbox.exists(unit))
        #expect(Set(await library.entries.map { "\($0.id) \($0.report?.versionCode ?? 0)" })
            == (setFirst ? ["\(SplitAPK.package) 1", "org.example.b 2"] : ["\(SplitAPK.package) 2", "org.example.b 1"]))

        // A unit's single APK goes once imported, as importFile discards its file; a set's files go with the unit.
        let second = try await library.importUnit(unit, replacing: [setFirst ? apk : set])
        #expect(second.map(\.decision) == (setFirst ? [.unchanged, .update] : [.update]))
        #expect(!sandbox.exists(unit))
    }

    @Test func aFreshSetThatAsksNamesTheFilesMinatoCannotCheck() async throws {
        // A split signed by an algorithm Minato cannot verify, naming the base's certificate (step b3).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unverifiable = SplitAPK.make(split: "config.de", v2: [TestSigner.Signer(.dsa, certificate: TestKey.ec.certificate)])
        let unit = try Sets.unit(["base.apk": Sets.base, "de.apk": unverifiable], in: sandbox.paths)

        let asked = try #require(try await library.importUnit(unit).first)

        #expect(asked.decision == .ask([.uncheckedContent]))
        #expect(asked.uncheckedFiles == ["de.apk"])
        let version = try #require(asked.asking)
        #expect(try await library.importUnit(unit, replacing: [version]).map(\.decision) == [.install])
    }

    @Test func aJoinAsksOnlyAboutTheUnitsFilesMinatoCannotCheck() async throws {
        // An installed APK Minato cannot check was asked about at its import, and its bytes are checked again
        // against library.json (b5b2), so a join names only the unit's own, by the names the user picked.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        func unverifiable(_ split: String) -> Data {
            SplitAPK.make(split: split, v2: [TestSigner.Signer(.dsa, certificate: TestKey.ec.certificate)])
        }
        let fresh = try Sets.unit(["base.apk": Sets.base, "arm64.apk": Sets.arm64, "de.apk": unverifiable("config.de")],
                                  in: sandbox.paths)
        let asked = try await library.importUnit(fresh)
        _ = try await library.importUnit(fresh, replacing: Set(asked.compactMap(\.asking)))
        #expect(await library.entries.first?.splits.map(\.splitName) == ["config.arm64_v8a", "config.de"])

        let fr = try #require(try await library.importUnit(Sets.unit(["fr.apk": unverifiable("config.fr")], in: sandbox.paths))
            .first)
        #expect(fr.decision == .ask([.uncheckedContent]))
        #expect(fr.uncheckedFiles == ["fr.apk"])

        let es = try await library.importUnit(Sets.unit(["es.apk": SplitAPK.make(split: "config.es")], in: sandbox.paths))
        #expect(es.map(\.decision) == [.reinstall])
    }

    @Test func aRefusedSetKeepsItsTextToShow() async throws {
        // Unlike a file that is no APK, whose error description the app turns into a reason of its own.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let results = try await library.importUnit(Sets.unit(["de.apk": SplitAPK.make(split: "config.de"),
                                                              "junk.apk": Data("junk".utf8)], in: sandbox.paths))

        let text = "Split APK “de.apk” needs its base APK. Import it together with its base APK."
        #expect(results.map(\.entry.refusal) == [nil, text])
        #expect(results.map(\.entry.state).last == .invalid(text))
        #expect(Set(await library.entries.map(\.refusal)) == [nil, text])
    }

    @Test func anIndexWithoutRefusalStillDecodes() async throws {
        // library.json from before step b7a, which older builds also write.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        _ = try await Library(paths: sandbox.paths).importUnit(Sets.unit(["de.apk": SplitAPK.make(split: "config.de")],
                                                                         in: sandbox.paths))
        var index = try #require(try JSONSerialization.jsonObject(with: Data(contentsOf: sandbox.paths.indexFile)) as? [String: Any])
        var entries = try #require(index["entries"] as? [[String: Any]])
        #expect(entries[0]["refusal"] != nil)
        entries[0]["refusal"] = nil
        index["entries"] = entries
        try JSONSerialization.data(withJSONObject: index).write(to: sandbox.paths.indexFile)

        let entry = try #require(try Library.entries(in: sandbox.paths).first)

        #expect(entry.refusal == nil)
        #expect(entry.state == .invalid("Split APK “de.apk” needs its base APK. Import it together with its base APK."))
    }

    @Test func refusalIsLeftOutWhenNilAndRoundTrips() throws {
        let entry = LibraryEntry(id: "file:de.apk", displayName: "de.apk", state: .invalid("refused"), report: nil, base: nil,
                                 splits: [], obbs: [], source: SourceFile(fileName: "de.apk", size: 1, modifiedMilliseconds: 0),
                                 iconFile: nil, importedAt: Date(timeIntervalSince1970: 0))
        let plain = try #require(try JSONSerialization.jsonObject(with: JSONEncoder().encode(entry)) as? [String: Any])
        #expect(plain["refusal"] == nil)

        var refused = entry
        refused.refusal = "refused"
        #expect(try JSONDecoder().decode(LibraryEntry.self, from: JSONEncoder().encode(refused)) == refused)
    }

    @Test func theLibrarysErrorsReadAsReasons() {
        // The app shows them after "could not be added:" and "could not be imported:".
        #expect(LibraryError.nothingToImport.localizedDescription == "it has no APK or OBB file")
        #expect(LibraryError.versionFolderInUse("0123456789abcdef").localizedDescription
            == "its version folder 0123456789abcdef is still in use")
        #expect(LibraryError.unsafePackageName("../x").localizedDescription == "“../x” is not a valid package name")
        #expect(ImportUnitError.tooManyFiles(600).localizedDescription
            == "it has 600 files, and Minato takes at most \(ImportUnit.maxFiles) at a time")
    }

    @Test func anAnswerReplacesOnlyTheQuestionAsShown() async throws {
        // "Replace" answered "1.0 is older than the installed 2.0". Meanwhile another pick put another signer's 3.0 in
        // its place, so the answer would give this version that app's data without a word about the other signer.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importFile(UpdateTests.picked(sandbox, library, SampleAPK.make(versionCode: 2), named: "v2.apk"))
        let unit = try await library.adopt([try sandbox.file(UpdateTests.olderVersion, named: "v1.apk")])
        let shown = try #require(try await library.importUnit(unit).first)
        #expect(shown.decision == .ask([.olderVersion]))
        let other = try await library.importFile(UpdateTests.picked(sandbox, library, UpdateTests.otherSigner, named: "v3.apk"),
                                                 replacing: true).entry

        let answered = try await library.importUnit(unit, replacing: [try #require(shown.asking)])

        #expect(answered.map(\.decision) == [.ask([.olderVersion, .otherSigner])])
        #expect(await library.entries == [other])
        #expect(sandbox.exists(unit.appendingPathComponent("v1.apk")))
    }

    @Test(arguments: [true, false])
    func keepInstalledOnAUnitDropsItsAPKsAndItsOBBs(withOBB: Bool) async throws {
        // As the unit's end after an import, and its OBBs with it, as the original stays where the user has it
        // (ADR-0049 d).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(UpdateTests.picked(sandbox, library, SampleAPK.make(versionCode: 2),
                                                                        named: "v2.apk")).entry
        let obb = "main.1.\(installed.id).obb"
        var pick = [try sandbox.file(UpdateTests.olderVersion, named: "v1.apk")]
        if withOBB { pick.append(try sandbox.file(Data("obb".utf8), named: obb)) }
        let unit = try await library.adopt(pick)
        #expect(try await library.importUnit(unit).map(\.decision) == [.ask([.olderVersion])])

        await library.keepInstalled(discarding: unit)

        #expect(!sandbox.exists(unit))
        #expect(try await library.scanUnits().isEmpty)
        #expect(await library.entries == [installed])
    }

    @Test func keepInstalledLeavesAFolderOutsideIncomingAlone() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let apk = try sandbox.file(SampleAPK.make(), named: "app.apk")

        await library.keepInstalled(discarding: apk.deletingLastPathComponent())

        #expect(sandbox.exists(apk))
    }

    /// Picks whose progress the app shows: one large APK, two small ones around a file that is no APK, or two sets.
    enum Pick: CaseIterable, Sendable {
        case large, mixed, sets

        var files: [String: Data] {
            switch self {
            case .large:
                // Three of the 4 MiB chunks the hash reads at a time.
                return ["app.apk": SampleAPK.make { zip in
                    var zip = zip
                    zip.add("assets/large.bin", Data(count: 9 << 20), method: .stored)
                    return TestSigner.sign(zip, v2: [TestSigner.Signer(.ec)])
                }]
            case .mixed:
                return ["b.apk": SampleAPK.make(package: "org.example.b"), "c.apk": Data("junk".utf8),
                        "d.apk": SampleAPK.make(package: "org.example.d")]
            case .sets:
                return ["base.apk": Sets.base, "arm64.apk": Sets.arm64, "other.apk": SplitAPK.make(package: "org.example.other"),
                        "other-arm64.apk": SplitAPK.make(package: "org.example.other", split: "config.arm64_v8a",
                                                         abis: ["arm64-v8a"])]
            }
        }
    }

    @Test(arguments: Pick.allCases)
    func aUnitShowsItsProgressAndReadsEachAPKOnce(pick: Pick) async throws {
        // The app shows a unit's progress on its tile: one large APK used to sit at 0 until it was done, and was read
        // twice, by the unit and by its import.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let read = Mutex<[String]>([])
        await library.observeHashing { url in read.withLock { $0.append(url.lastPathComponent) } }
        let files = pick.files
        let unit = try Sets.unit(files, in: sandbox.paths)
        let values = Mutex<[Double]>([])

        _ = try await library.importUnit(unit) { value in values.withLock { $0.append(value) } }

        let shown = values.withLock { $0 }
        #expect(shown.first == 0)
        #expect(shown.last == 1)
        #expect(zip(shown, shown.dropFirst()).allSatisfy { $0 <= $1 })
        #expect(shown.contains { $0 > 0 && $0 < 0.5 }) // while the unit reads its APKs
        #expect(shown.contains { $0 > 0.5 && $0 < 1 }) // while it imports them
        #expect(read.withLock { $0 } == files.keys.sorted { $0.utf8.lexicographicallyPrecedes($1.utf8) })
    }
}
