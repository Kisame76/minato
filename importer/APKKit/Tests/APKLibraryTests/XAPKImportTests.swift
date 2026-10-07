import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Synchronization
import Testing

@testable import APKLibrary

/// An XAPK in a unit (ADR-0049 c2): its APKs install as a container's do, its OBBs are staged in the version's folder
/// and moved into data/<package>/obb/ after the switch, where one replaces the installed OBB of its kind (decision 4)
/// unless the version stays and it is older (App's review), and what it leaves out is named on the entry.
@Suite("XAPK import")
struct XAPKImportTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Sets = SetImportTests

    static let package = SplitAPK.package
    static func obbPath(_ name: String, in folder: String = package) -> String { "Android/obb/\(folder)/\(name)" }

    /// An XAPK as APKPure writes one: manifest.json with an expansion for each OBB, the APKs, the OBBs and `extra`.
    static func xapk(_ apks: [(String, Data)], obbs: [(String, Data)] = [], extra: [(String, Data)] = []) -> Data {
        let manifest: [String: Any] = [
            "xapk_version": 2, "package_name": package, "version_code": "1",
            "split_apks": apks.map { ["file": $0.0, "id": $0.0] },
            "expansions": obbs.map { ["file": $0.0, "install_location": "EXTERNAL_STORAGE", "install_path": $0.0] },
        ]
        let json = try! JSONSerialization.data(withJSONObject: manifest, options: [.sortedKeys])
        return ContainerImportTests.zip([("manifest.json", json)] + apks + obbs + extra)
    }

    /// A unit with one XAPK of the base APK `base` and the OBBs named `obbs`.
    static func unit(_ base: Data = Sets.base, obbs: [(String, Data)], in paths: LibraryPaths) throws -> URL {
        try Sets.unit(["game.xapk": xapk([("\(package).apk", base)], obbs: obbs.map { (obbPath($0.0), $0.1) })], in: paths)
    }

    static func obbFolder(_ paths: LibraryPaths, _ package: String = package) -> URL {
        paths.dataDirectory(for: package).appendingPathComponent("obb", isDirectory: true)
    }

    static func obbFiles(_ paths: LibraryPaths) throws -> [String] {
        try FileManager.default.contentsOfDirectory(atPath: obbFolder(paths).path).sorted()
    }

    static func content(_ paths: LibraryPaths, _ name: String) throws -> Data {
        try Data(contentsOf: obbFolder(paths).appendingPathComponent(name))
    }

    static func obb(_ name: String, _ data: Data) -> PackageFile {
        PackageFile(fileName: name, splitName: nil, size: UInt64(data.count), sha256: Sets.sha256(data))
    }

    static func name(_ kind: String, _ version: Int) -> String { "\(kind).\(version).\(package).obb" }

    /// Whether `url` itself carries the attribute that excludes it from backups, read without following a link and
    /// past Foundation's cache of resource values.
    static func isMarkedExcluded(_ url: URL) -> Bool {
        getxattr(url.path, "com.apple.metadata:com_apple_backup_excludeItem", nil, 0, 0, XATTR_NOFOLLOW) >= 0
    }
    static func data(_ kind: String, _ version: Int) -> Data { Data("\(kind) \(version)".utf8) }

    // MARK: - The APKs and the OBBs

    @Test func aSplitXAPKGivesItsEntryWithItsSplits() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let data = Self.xapk([("\(Self.package).apk", Sets.base), ("config.arm64_v8a.apk", Sets.arm64)])
        let unit = try Sets.unit(["game.xapk": data], in: sandbox.paths)
        #expect(try await library.scanUnits().map(\.lastPathComponent) == [unit.lastPathComponent])

        let results = try await library.importUnit(unit)

        let entry = try #require(results.first?.entry)
        #expect(results.map(\.decision) == [.install])
        #expect(entry.id == Self.package && entry.state == .ready)
        #expect(entry.splits.map(\.fileName) == ["split_config.arm64_v8a.apk"])
        #expect(entry.source.fileName == "game.xapk" && entry.source.size == UInt64(data.count))
        #expect(entry.obbs.isEmpty && entry.ignoredFiles == nil)
        #expect(!sandbox.exists(unit))
    }

    @Test func anOBBXAPKMovesItsOBBsIntoTheAppsOBBFolder() async throws {
        // Staged in the new version's folder, which keeps none of them once they moved.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1)), (Self.name("patch", 1), Self.data("patch", 1))],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        let entry = try #require(results.first?.entry)
        let obbs = [Self.obb(Self.name("main", 1), Self.data("main", 1)), Self.obb(Self.name("patch", 1), Self.data("patch", 1))]
        #expect(results.map(\.decision) == [.install])
        #expect(results.first?.installedOBBs == obbs)
        #expect(entry.state == .ready && entry.obbs == obbs)
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 1), Self.name("patch", 1)])
        #expect(try Self.content(sandbox.paths, Self.name("main", 1)) == Self.data("main", 1))
        #expect(try Sets.versionFiles(entry, sandbox.paths) == ["base.apk", "lib", "signers.json"])
        #expect(await library.entries == [entry])
        #expect(!sandbox.exists(unit))
    }

    @Test func anXAPKsProgressCountsItsOBBs() async throws {
        // The first half of a unit's progress reads its APKs and OBBs by their sizes, so an OBB larger than its APK
        // moves the tile and never takes it past half before the imports.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Self.unit(obbs: [(Self.name("main", 1), Data(count: 1 << 18))], in: sandbox.paths)
        let values = Mutex<[Double]>([])

        _ = try await library.importUnit(unit) { value in values.withLock { $0.append(value) } }

        let shown = values.withLock { $0 }
        #expect(shown.first == 0 && shown.last == 1)
        #expect(zip(shown, shown.dropFirst()).allSatisfy { $0 <= $1 })
        #expect(shown.contains { $0 > 0 && $0 < 0.5 })
    }

    @Test func theOBBFolderIsExcludedFromBackups() async throws {
        // OBBs are a store's to give again, not the user's to back up (the M5 plan's decision 9); the data is.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        _ = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths))

        #expect(Self.isMarkedExcluded(Self.obbFolder(sandbox.paths)))
        #expect(!Self.isMarkedExcluded(sandbox.paths.dataDirectory(for: Self.package)))
    }

    @Test func theStagedOBBsAreExcludedFromBackupsToo() async throws {
        // The lead's reviewer: they wait in the new version's folder until the switch, and a crash leaves them there.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        struct Interrupted: Error {}
        await library.interruptBeforeTheSwitch { throw Interrupted() }
        let unit = try Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths)

        await #expect(throws: Interrupted.self) { try await library.importUnit(unit) }

        let staging = sandbox.paths.installDirectory(for: Self.package)
            .appendingPathComponent(String(Sets.sha256(Sets.base).prefix(16)), isDirectory: true)
            .appendingPathComponent("obb", isDirectory: true)
        #expect(sandbox.exists(staging.appendingPathComponent(Self.name("main", 1))))
        #expect(Self.isMarkedExcluded(staging))
    }

    @Test func whatTheXAPKLeavesOutIsNamedOnTheEntry() async throws {
        // What the reader names (c1), then an OBB of a package without an APK here.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let other = "main.3.com.other.app.obb"
        let unit = try Sets.unit(["game.xapk": Self.xapk([("\(Self.package).apk", Sets.base)],
                                                         obbs: [(Self.obbPath(other, in: "com.other.app"), Self.data("main", 3))],
                                                         extra: [(Self.obbPath("readme.txt"), Data("x".utf8))])],
                                 in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.obbs.isEmpty)
        #expect(entry.ignoredFiles == [
            IgnoredFile(path: Self.obbPath("readme.txt"),
                        reason: "its name is not main.<version>.<package>.obb or patch.<version>.<package>.obb"),
            IgnoredFile(path: Self.obbPath(other, in: "com.other.app"), reason: "it is for com.other.app, which has no APK here"),
        ])
        #expect(!sandbox.exists(Self.obbFolder(sandbox.paths, "com.other.app")))
        #expect(!sandbox.exists(unit))
    }

    @Test func anOBBOfAPackageWithoutAnAPKHereIsNeverTakenOut() async throws {
        // The lead's reviewer: an XAPK never brings an OBB for another app, so one of a package without an APK in it is
        // named before anything is taken out. Its bytes are damaged, so taking it out would refuse the whole XAPK.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let other = Self.obbPath("main.3.com.other.app.obb", in: "com.other.app")
        var zip = ZipBuilder()
        zip.add("manifest.json", Data(#"{"xapk_version": 2, "package_name": "\#(Self.package)"}"#.utf8), method: .stored)
        zip.add("\(Self.package).apk", Sets.base, method: .stored)
        zip.add(other, Self.data("main", 3), method: .stored)
        let output = zip.build()
        var data = output.data
        data[try #require(output.dataOffsets[other])] ^= 0xFF
        let unit = try Sets.unit(["game.xapk": data], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        let entry = try #require(results.first?.entry)
        #expect(results.map(\.decision) == [.install])
        #expect(entry.id == Self.package && entry.state == .ready)
        #expect(entry.ignoredFiles == [IgnoredFile(path: other, reason: "it is for com.other.app, which has no APK here")])
        #expect(!sandbox.exists(Self.obbFolder(sandbox.paths, "com.other.app")))
        #expect(!sandbox.exists(unit))
    }

    @Test func aDamagedOBBRefusesItsXAPKAndLeavesNothingInTheUnit() async throws {
        // Its OBBs are taken out after its APKs; the XAPK is refused as a whole, and the APKs' folder goes with it.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let path = Self.obbPath(Self.name("main", 1))
        var zip = ZipBuilder()
        zip.add("manifest.json", Data(#"{"xapk_version": 2, "package_name": "\#(Self.package)"}"#.utf8), method: .stored)
        zip.add("\(Self.package).apk", Sets.base, method: .stored)
        zip.add(path, Self.data("main", 1), method: .stored)
        let output = zip.build()
        var data = output.data
        data[try #require(output.dataOffsets[path])] ^= 0xFF
        let unit = try Sets.unit(["game.xapk": data], in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.id == "file:game.xapk")
        #expect(entry.refusal == "“\(path)” in it is damaged: its checksum does not match")
        #expect(!sandbox.exists(Self.obbFolder(sandbox.paths)))
        #expect(!sandbox.exists(unit))
    }

    @Test func theXAPKsNamesGoOnOneOfItsEntries() async throws {
        // The lead's reviewer: copied onto each entry of an XAPK with many apps, they made library.json grow with every
        // store.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let apks = [("\(Self.package).apk", Sets.base)]
            + ["com.other.app", "com.third.app"].map { (name: String) in ("\(name).apk", SplitAPK.make(package: name)) }
        let unit = try Sets.unit(["game.xapk": Self.xapk(apks, extra: [(Self.obbPath("readme.txt"), Data("x".utf8))])],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.map(\.decision) == [.install, .install, .install])
        #expect(results.compactMap(\.entry.ignoredFiles).map(\.count) == [1])
        #expect(await library.entries.compactMap(\.ignoredFiles).count == 1)
    }

    @Test func theImportsOwnNamesAreCountedPastTheirCapAfterTheContainers() async throws {
        // The reader caps what the container leaves out (c1); what the import leaves out besides follows it, capped
        // the same way, so each last name counts the rest of its own list.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let junk = (1...66).map { (Self.obbPath("note\($0).txt"), Data("x".utf8)) }
        let others = (1...70).map { (Self.obbPath("main.1.com.other\($0).obb", in: "com.other\($0)"), Self.data("main", 1)) }
        let unit = try Sets.unit(["game.xapk": Self.xapk([("\(Self.package).apk", Sets.base)], obbs: others, extra: junk)],
                                 in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        let names = try #require(entry.ignoredFiles)
        #expect(names.count == 2 * IgnoredFile.maxCount)
        #expect(names[62].path == Self.obbPath("note63.txt"))
        #expect(names[63] == IgnoredFile(path: "…", reason: "3 more files are left out"))
        #expect(names[64] == IgnoredFile(path: others[0].0, reason: "it is for com.other1, which has no APK here"))
        #expect(names[126].path == others[62].0)
        #expect(names[127] == IgnoredFile(path: "…", reason: "7 more files are left out"))
        #expect(entry.state == .ready && entry.obbs.isEmpty)
    }

    // MARK: - Which OBB stays (decision 4, App's review)

    @Test func anUpdatesOBBReplacesAnOlderOneOfItsKindAndEverythingElseStays() async throws {
        // Where the version changes, a newer OBB replaces the installed one of its kind and an older one stays out (the
        // lead's rule): here main 2 over main 1, then main 1 named beside main 2. The installed patch and files the app
        // wrote stay, one named like another app's main OBB among them.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1)),
                                                          (Self.name("patch", 1), Self.data("patch", 1))], in: sandbox.paths))
        try Data("the app's".utf8).write(to: Self.obbFolder(sandbox.paths).appendingPathComponent("cache.bin"))
        let othersName = "main.1.com.other.app.obb"
        try Data("the app's".utf8).write(to: Self.obbFolder(sandbox.paths).appendingPathComponent(othersName))

        let v2 = try await library.importUnit(Self.unit(SplitAPK.make(versionCode: 2), obbs: [(Self.name("main", 2), Self.data("main", 2))],
                                                        in: sandbox.paths))

        #expect(v2.map(\.decision) == [.update])
        #expect(v2.first?.entry.obbs == [Self.obb(Self.name("patch", 1), Self.data("patch", 1)),
                                        Self.obb(Self.name("main", 2), Self.data("main", 2))])
        #expect(try Self.obbFiles(sandbox.paths) == ["cache.bin", othersName, Self.name("main", 2), Self.name("patch", 1)])
        let v3 = try await library.importUnit(Self.unit(SplitAPK.make(versionCode: 3), obbs: [(Self.name("main", 1), Self.data("main", 1))],
                                                        in: sandbox.paths))
        #expect(v3.map(\.decision) == [.update])
        #expect(v3.first?.installedOBBs == [])
        #expect(try Self.obbFiles(sandbox.paths) == ["cache.bin", othersName, Self.name("main", 2), Self.name("patch", 1)])
        #expect(v3.first?.entry.obbs.map(\.fileName) == [Self.name("patch", 1), Self.name("main", 2)])
        #expect(v3.first?.entry.ignoredFiles == [IgnoredFile(path: Self.obbPath(Self.name("main", 1)),
                                                             reason: "it is older than the installed \(Self.name("main", 2))")])
    }

    @Test func theSameVersionWithAnOBBAddsTheOBB() async throws {
        // The bare APK first, then the same APK in an XAPK: nothing to install but its OBB.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try #require(try await library.importUnit(Sets.unit(["base.apk": Sets.base], in: sandbox.paths)).first?.entry)
        let unit = try Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        let entry = try #require(results.first?.entry)
        #expect(results.map(\.decision) == [.unchanged])
        #expect(results.first?.installedOBBs == [Self.obb(Self.name("main", 1), Self.data("main", 1))])
        #expect(entry.folder == installed.folder)
        #expect(entry.obbs == [Self.obb(Self.name("main", 1), Self.data("main", 1))])
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 1)])
        #expect(try Sets.versionFiles(entry, sandbox.paths) == ["base.apk", "lib", "signers.json"])
        #expect(await library.entries == [entry])
        #expect(!sandbox.exists(unit))
    }

    @Test func theSameVersionFromAnXAPKNamesWhatItLeavesOut() async throws {
        // Nothing to install, yet the entry names what this XAPK leaves out.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Sets.unit(["base.apk": Sets.base], in: sandbox.paths))
        let unit = try Sets.unit(["game.xapk": Self.xapk([("\(Self.package).apk", Sets.base)],
                                                         extra: [(Self.obbPath("readme.txt"), Data("x".utf8))])],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.map(\.decision) == [.unchanged])
        let named = [IgnoredFile(path: Self.obbPath("readme.txt"),
                                 reason: "its name is not main.<version>.<package>.obb or patch.<version>.<package>.obb")]
        #expect(results.first?.entry.ignoredFiles == named)
        #expect(await library.entries.first?.ignoredFiles == named)
    }

    @Test func theSameVersionKeepsItsOBBAndAsksBeforeOtherBytes() async throws {
        // Where the version stays, the installed main 9 is what the working app reads, so main 7 stays out. The same OBB
        // again moves nothing, and other bytes under its name replace it only once the user answers (the lead's rule).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 9), Self.data("main", 9))], in: sandbox.paths))
        let file = Self.obbFolder(sandbox.paths).appendingPathComponent(Self.name("main", 9))
        let inode = try FileManager.default.attributesOfItem(atPath: file.path)[.systemFileNumber] as? Int

        let older = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 7), Self.data("main", 7))], in: sandbox.paths))

        #expect(older.map(\.decision) == [.unchanged])
        #expect(older.first?.installedOBBs == [])
        #expect(older.first?.entry.obbs == [Self.obb(Self.name("main", 9), Self.data("main", 9))])
        #expect(older.first?.entry.ignoredFiles == [IgnoredFile(path: Self.obbPath(Self.name("main", 7)),
                                                                reason: "the same version keeps the installed \(Self.name("main", 9))")])
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 9)])
        let same = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 9), Self.data("main", 9))], in: sandbox.paths))
        #expect(same.first?.installedOBBs == [])
        #expect(try FileManager.default.attributesOfItem(atPath: file.path)[.systemFileNumber] as? Int == inode)
        let other = Data("main 9, rebuilt".utf8)
        let rebuilt = try Self.unit(obbs: [(Self.name("main", 9), other)], in: sandbox.paths)
        let asked = try await library.importUnit(rebuilt)
        #expect(asked.map(\.decision) == [.ask([.otherOBB])])
        #expect(asked.first?.otherOBBs == [Self.name("main", 9)])
        #expect(try Self.content(sandbox.paths, Self.name("main", 9)) == Self.data("main", 9))
        #expect(sandbox.exists(rebuilt))
        let shown = AskingVersion(package: Self.package, versionCode: 1, warnings: [.otherOBB])
        let replaced = try await library.importUnit(rebuilt, replacing: [shown])
        #expect(replaced.map(\.decision) == [.unchanged])
        #expect(replaced.first?.installedOBBs == [Self.obb(Self.name("main", 9), other)])
        #expect(try Self.content(sandbox.paths, Self.name("main", 9)) == other)
        #expect(await library.entries.first?.obbs == [Self.obb(Self.name("main", 9), other)])
    }

    @Test func aReinstallKeepsANewerOBBAsTheSameBytesDo() async throws {
        // The same version with other bytes, here the base with a split, keeps the versionCode, so its older OBB stays
        // out as well.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 9), Self.data("main", 9))], in: sandbox.paths))
        let unit = try Sets.unit(["game.xapk": Self.xapk([("\(Self.package).apk", Sets.base), ("config.arm64_v8a.apk", Sets.arm64)],
                                                         obbs: [(Self.obbPath(Self.name("main", 7)), Self.data("main", 7))])],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.map(\.decision) == [.reinstall])
        #expect(results.first?.installedOBBs == [])
        #expect(results.first?.entry.obbs == [Self.obb(Self.name("main", 9), Self.data("main", 9))])
        #expect(results.first?.entry.ignoredFiles == [IgnoredFile(path: Self.obbPath(Self.name("main", 7)),
                                                                  reason: "the same version keeps the installed \(Self.name("main", 9))")])
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 9)])
    }

    @Test func keepInstalledLetsAnAskingXAPKGo() async throws {
        // c2's set-check: after "Keep Installed" the unit goes with its XAPK and both folders it took out, and the
        // installed OBB stays as it is.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths))
        let unit = try Self.unit(obbs: [(Self.name("main", 1), Data("main 1, other".utf8))], in: sandbox.paths)
        #expect(try await library.importUnit(unit).map(\.decision) == [.ask([.otherOBB])])

        await library.keepInstalled(discarding: unit)

        #expect(!sandbox.exists(unit))
        #expect(try Self.content(sandbox.paths, Self.name("main", 1)) == Self.data("main", 1))
    }

    @Test func theSameVersionNeverRemovesAnOBB() async throws {
        // A newer main from the same version stays out as an older one does: the working app reads main 7 (the lead's
        // rule).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 7), Self.data("main", 7))], in: sandbox.paths))

        let results = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 9), Self.data("main", 9))], in: sandbox.paths))

        #expect(results.map(\.decision) == [.unchanged])
        #expect(results.first?.installedOBBs == [])
        #expect(results.first?.entry.ignoredFiles == [IgnoredFile(path: Self.obbPath(Self.name("main", 9)),
                                                                  reason: "the same version keeps the installed \(Self.name("main", 7))")])
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 7)])
    }

    @Test func anOBBWhoseBytesAreThereIsRecordedWithoutAQuestion() async throws {
        // The same bytes under its name, here not recorded, ask nothing and move nothing.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Sets.unit(["base.apk": Sets.base], in: sandbox.paths))
        try Self.data("main", 1).write(to: Self.obbFolder(sandbox.paths).appendingPathComponent(Self.name("main", 1)))

        let results = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths))

        #expect(results.map(\.decision) == [.unchanged])
        #expect(results.first?.installedOBBs == [])
        #expect(results.first?.entry.obbs == [Self.obb(Self.name("main", 1), Self.data("main", 1))])
    }

    @Test func aQuestionForAnotherReasonNamesTheOBBsToo() async throws {
        // An older version asks anyway; its OBB with other bytes under the installed name joins the same question.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(SplitAPK.make(versionCode: 2), obbs: [(Self.name("main", 1), Self.data("main", 1))],
                                                   in: sandbox.paths))

        let asked = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Data("main 1, other".utf8))], in: sandbox.paths))

        #expect(asked.map(\.decision) == [.ask([.olderVersion, .otherOBB])])
        #expect(asked.first?.otherOBBs == [Self.name("main", 1)])
    }

    @Test func aFreshInstallTakesItsOBBFolderAsItFindsIt() async throws {
        // No installed app keeps anything there, so its OBB goes over a file of its name without a question.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        try FileManager.default.createDirectory(at: Self.obbFolder(sandbox.paths), withIntermediateDirectories: true)
        try Data("left over".utf8).write(to: Self.obbFolder(sandbox.paths).appendingPathComponent(Self.name("main", 1)))

        let results = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths))

        #expect(results.map(\.decision) == [.install])
        #expect(try Self.content(sandbox.paths, Self.name("main", 1)) == Self.data("main", 1))
    }

    @Test func anUpdateAsksBeforeOtherBytesUnderAnInstalledOBBsName() async throws {
        // The name carries the version its file was made for, so other bytes under it ask whatever the APK's version
        // (the lead's rule); the default keeps the installed file and version.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths))
        let other = Data("main 1, other".utf8)
        let unit = try Self.unit(SplitAPK.make(versionCode: 2), obbs: [(Self.name("main", 1), other)], in: sandbox.paths)

        let asked = try await library.importUnit(unit)

        #expect(asked.map(\.decision) == [.ask([.otherOBB])])
        #expect(asked.first?.otherOBBs == [Self.name("main", 1)])
        #expect(await library.entries.first?.report?.versionCode == 1)
        #expect(try Self.content(sandbox.paths, Self.name("main", 1)) == Self.data("main", 1))
        let shown = AskingVersion(package: Self.package, versionCode: 2, warnings: [.otherOBB])
        let replaced = try await library.importUnit(unit, replacing: [shown])
        #expect(replaced.map(\.decision) == [.update])
        #expect(replaced.first?.installedOBBs == [Self.obb(Self.name("main", 1), other)])
        #expect(try Self.content(sandbox.paths, Self.name("main", 1)) == other)
        #expect(!sandbox.exists(unit))
    }

    @Test func anOBBGoesWithTheSetOfItsOwnXAPK() async throws {
        // Another set of its package in the unit, here 1.0 beside the XAPK's 2.0, never takes it.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Sets.unit(["base.apk": Sets.base,
                                  "game.xapk": Self.xapk([("\(Self.package).apk", SplitAPK.make(versionCode: 2))],
                                                         obbs: [(Self.obbPath(Self.name("main", 2)), Self.data("main", 2))])],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.map(\.decision) == [.install, .update])
        #expect(results.map(\.installedOBBs) == [[], [Self.obb(Self.name("main", 2), Self.data("main", 2))]])
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 2)])
    }

    @Test func eachAppOfAnXAPKGetsItsOwnOBBs() async throws {
        // Two apps in one XAPK: each OBB goes with the set its name gives.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let other = "com.other.app"
        let unit = try Sets.unit(["game.xapk": Self.xapk([("\(Self.package).apk", Sets.base), ("\(other).apk", SplitAPK.make(package: other))],
                                                         obbs: [(Self.obbPath(Self.name("main", 1)), Self.data("main", 1)),
                                                                (Self.obbPath("main.1.\(other).obb", in: other), Data("other".utf8))])],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.map(\.decision) == [.install, .install])
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 1)])
        #expect(try FileManager.default.contentsOfDirectory(atPath: Self.obbFolder(sandbox.paths, other).path)
            == ["main.1.\(other).obb"])
    }

    @Test func anOBBOfAnAPKThatJoinsNoSetStaysOutAndIsNamed() async throws {
        // Its package has an APK in the XAPK, a split Android's parse refuses, so no set takes the OBB, and the other
        // app's entry from the XAPK names it.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let other = "com.other.app"
        let usesSdk = AXMLBuilder.Element("uses-sdk", [.android("minSdkVersion", 0x0101_020C, .int(99))])
        let path = Self.obbPath("main.1.\(other).obb", in: other)
        let unit = try Sets.unit(["game.xapk": Self.xapk([("\(Self.package).apk", Sets.base),
                                                          ("config.de.apk", SplitAPK.make(package: other, split: "config.de",
                                                                                          elements: [usesSdk]))],
                                                         obbs: [(path, Data("other".utf8))])],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        let entry = try #require(results.first { $0.entry.id == Self.package }?.entry)
        #expect(entry.ignoredFiles == [IgnoredFile(path: path, reason: "it is for \(other), which has no APK here")])
        #expect(!sandbox.exists(Self.obbFolder(sandbox.paths, other)))
    }

    @Test func aRecordedOBBThatIsMissingMovesIn() async throws {
        // A crash between the switch and the move leaves the OBB recorded but not there; the unit's next import, the
        // same version, moves it in.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths))
        try FileManager.default.removeItem(at: Self.obbFolder(sandbox.paths).appendingPathComponent(Self.name("main", 1)))

        let results = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths))

        #expect(results.map(\.decision) == [.unchanged])
        #expect(results.first?.installedOBBs == [Self.obb(Self.name("main", 1), Self.data("main", 1))])
        #expect(try Self.content(sandbox.paths, Self.name("main", 1)) == Self.data("main", 1))
    }

    @Test func anOBBsVersionCountsAsANumber() {
        // However many digits it has, so 10 is newer than 9, and leading zeros count for nothing.
        #expect(Library.isNewer("10", than: "9") && !Library.isNewer("9", than: "10"))
        #expect(!Library.isNewer("007", than: "7") && !Library.isNewer("7", than: "007"))
        #expect(Library.isNewer("010", than: "9"))
    }

    @Test func aFolderUnderAnOBBsNameLeavesTheInstalledOneAlone() async throws {
        // The lead's reviewer: a folder the app made under main 2's name, locked, so no move gets past it. The update
        // installs without main 2, which is named, and main 1 stays where the app reads it.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths))
        let taken = Self.obbFolder(sandbox.paths).appendingPathComponent(Self.name("main", 2))
        try FileManager.default.createDirectory(at: taken, withIntermediateDirectories: false, attributes: [.posixPermissions: 0])
        defer { try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: taken.path) }
        let unit = try Self.unit(SplitAPK.make(versionCode: 2), obbs: [(Self.name("main", 2), Self.data("main", 2))],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.map(\.decision) == [.update])
        #expect(results.first?.installedOBBs == [])
        #expect(results.first?.entry.obbs == [Self.obb(Self.name("main", 1), Self.data("main", 1))])
        #expect(results.first?.entry.ignoredFiles == [IgnoredFile(path: Self.obbPath(Self.name("main", 2)),
                                                                  reason: "the app’s OBB folder holds something else under its name")])
        #expect(try Self.content(sandbox.paths, Self.name("main", 1)) == Self.data("main", 1))
        #expect(!sandbox.exists(unit))
    }

    @Test func theNewOBBsMoveInBeforeTheOldOnesGo() async throws {
        // The lead's reviewer: an interruption between the two halves leaves both, never neither.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths))
        struct Interrupted: Error {}
        await library.interruptBetweenTheOBBMoves { throw Interrupted() }
        let unit = try Self.unit(SplitAPK.make(versionCode: 2), obbs: [(Self.name("main", 2), Self.data("main", 2))],
                                 in: sandbox.paths)

        await #expect(throws: Interrupted.self) { try await library.importUnit(unit) }

        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 1), Self.name("main", 2)])
        #expect(try Self.content(sandbox.paths, Self.name("main", 2)) == Self.data("main", 2))
    }

    // MARK: - Links and other things in the OBB folder (the lead's reviewer)

    @Test func aLinkUnderAnOBBsNameIsReplacedNeverFollowed() async throws {
        // A guest may put links into its OBB folder: a move replaces one as a link, and an installed OBB that became one
        // stays a link, so nothing reaches the file they name.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths))
        let secret = sandbox.root.appendingPathComponent("secret.txt")
        try Data("secret".utf8).write(to: secret)
        let folder = Self.obbFolder(sandbox.paths)
        try FileManager.default.removeItem(at: folder.appendingPathComponent(Self.name("main", 1)))
        for name in [Self.name("main", 1), Self.name("main", 2)] {
            try FileManager.default.createSymbolicLink(at: folder.appendingPathComponent(name), withDestinationURL: secret)
        }
        let unit = try Self.unit(SplitAPK.make(versionCode: 2), obbs: [(Self.name("main", 2), Self.data("main", 2))],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.first?.installedOBBs == [Self.obb(Self.name("main", 2), Self.data("main", 2))])
        #expect(try Data(contentsOf: secret) == Data("secret".utf8))
        #expect(try Self.content(sandbox.paths, Self.name("main", 2)) == Self.data("main", 2))
        let kinds = try [Self.name("main", 1), Self.name("main", 2)].map {
            try FileManager.default.attributesOfItem(atPath: folder.appendingPathComponent($0).path)[.type] as? FileAttributeType
        }
        #expect(kinds == [.typeSymbolicLink, .typeRegular])
    }

    @Test func aLinkInTheOBBFoldersPlaceGoesAsALink() async throws {
        // The guest's mounts leave it no way to put one there; should one stand there all the same, it is removed
        // without being followed, what lies behind it counts for nothing, and the OBBs go into a folder of their own.
        // Whether the backup flag reaches the folder behind it is not asked here: the flag the first import set is
        // written again after its call returns, by path, and can follow the link this test puts there at once
        // [verified: a probe on macOS 27, 27 of 200 times].
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths))
        let elsewhere = sandbox.root.appendingPathComponent("elsewhere", isDirectory: true)
        try FileManager.default.createDirectory(at: elsewhere, withIntermediateDirectories: true)
        try Data("not the app's".utf8).write(to: elsewhere.appendingPathComponent(Self.name("main", 2)))
        let folder = Self.obbFolder(sandbox.paths)
        try FileManager.default.removeItem(at: folder)
        try FileManager.default.createSymbolicLink(at: folder, withDestinationURL: elsewhere)
        let unit = try Self.unit(SplitAPK.make(versionCode: 2), obbs: [(Self.name("main", 2), Self.data("main", 2))],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.first?.installedOBBs == [Self.obb(Self.name("main", 2), Self.data("main", 2))])
        #expect(try FileManager.default.contentsOfDirectory(atPath: elsewhere.path) == [Self.name("main", 2)])
        #expect(try Data(contentsOf: elsewhere.appendingPathComponent(Self.name("main", 2))) == Data("not the app's".utf8))
        #expect(try FileManager.default.attributesOfItem(atPath: folder.path)[.type] as? FileAttributeType == .typeDirectory)
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 2)])
    }

    @Test(arguments: [false, true])
    func aFileInTheOBBFoldersPlaceStopsTheImportBeforeAnythingChanges(sameVersion: Bool) async throws {
        // The lead's call: it stays as it is, the import stops with the reason, and the unit waits.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try #require(try await library.importUnit(Sets.unit(["base.apk": Sets.base], in: sandbox.paths)).first?.entry)
        let folder = Self.obbFolder(sandbox.paths)
        try FileManager.default.removeItem(at: folder)
        try Data("a file".utf8).write(to: folder)
        let unit = try Self.unit(sameVersion ? Sets.base : SplitAPK.make(versionCode: 2),
                                 obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths)

        await #expect(throws: LibraryError.obbFolderIsNoFolder(Self.package)) { try await library.importUnit(unit) }

        #expect(try Data(contentsOf: folder) == Data("a file".utf8))
        #expect(await library.entries == [installed])
        #expect(sandbox.exists(unit))
    }

    @Test func aStagedOBBACrashLeftIsReplaced() async throws {
        // App's review: a crash between the switch and the move leaves a clone in the version's obb/; the unit, still
        // in incoming/, comes again as the same version, and its OBB installs over the leftover.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try #require(try await library.importUnit(Sets.unit(["base.apk": Sets.base], in: sandbox.paths)).first?.entry)
        let staging = sandbox.paths.versionDirectory(for: installed).appendingPathComponent("obb", isDirectory: true)
        try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
        try Data("left over".utf8).write(to: staging.appendingPathComponent(Self.name("main", 1)))

        let results = try await library.importUnit(Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths))

        #expect(results.first?.installedOBBs == [Self.obb(Self.name("main", 1), Self.data("main", 1))])
        #expect(try Self.content(sandbox.paths, Self.name("main", 1)) == Self.data("main", 1))
        #expect(!sandbox.exists(staging))
    }

    // MARK: - Questions and refusals

    @Test func aVersionThatAsksKeepsItsOBBsWithTheUnitUntilAnswered() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Sets.unit(["base.apk": SplitAPK.make(versionCode: 2)], in: sandbox.paths))
        let older = try Self.unit(obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths)

        #expect(try await library.importUnit(older).map(\.decision) == [.ask([.olderVersion])])
        #expect(try Self.obbFiles(sandbox.paths).isEmpty)
        #expect(sandbox.exists(older))
        let shown = AskingVersion(package: Self.package, versionCode: 1, warnings: [.olderVersion])
        #expect(try await library.importUnit(older, replacing: [shown]).map(\.decision) == [.update])
        #expect(try Self.obbFiles(sandbox.paths) == [Self.name("main", 1)])
        #expect(await library.entries.first?.obbs == [Self.obb(Self.name("main", 1), Self.data("main", 1))])
        #expect(!sandbox.exists(older))
    }

    @Test func anIncompatibleAppGetsNoOBBFromTheSameVersion() async throws {
        // As an incompatible version installs no OBB, the same version from an XAPK adds none.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let incompatible = SplitAPK.make(abis: ["armeabi-v7a"])
        _ = try await library.importUnit(Sets.unit(["base.apk": incompatible], in: sandbox.paths))
        let unit = try Self.unit(incompatible, obbs: [(Self.name("main", 1), Self.data("main", 1))], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.map(\.decision) == [.unchanged])
        #expect(results.first?.installedOBBs == [])
        #expect(results.first?.entry.obbs == [])
        #expect(!sandbox.exists(Self.obbFolder(sandbox.paths)))
        #expect(!sandbox.exists(unit))
    }

    @Test func aRejectedVersionInstallsNoOBB() async throws {
        // An incompatible new version leaves the ready one as it is (ADR-0034), and its OBBs stay out.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Sets.unit(["base.apk": Sets.base], in: sandbox.paths))
        let unit = try Self.unit(SplitAPK.make(versionCode: 2, abis: ["armeabi-v7a"]),
                                 obbs: [(Self.name("main", 2), Self.data("main", 2))], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.map(\.decision) == [.reject(.incompatible)])
        #expect(results.first?.installedOBBs == [])
        #expect(try Self.obbFiles(sandbox.paths).isEmpty)
        #expect(!sandbox.exists(unit))
    }
}
