import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

/// A container's entries taken out chunk by chunk under names of ours, each checked against its CRC-32 and declared
/// size, after the free space is checked, into a folder that exists whole or not at all (ADR-0049 decision 6).
@Suite("Container extraction")
struct ContainerExtractionTests {
    /// An import unit's folder of its own, removed with the test.
    final class Unit {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("unit-\(UUID().uuidString)", isDirectory: true)
        var destination: URL { url.appendingPathComponent("contents", isDirectory: true) }

        init() throws { try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true) }
        deinit { try? FileManager.default.removeItem(at: url) }

        func file(_ data: Data, name: String = "game.apks") throws -> URL {
            let file = url.appendingPathComponent(name)
            try data.write(to: file)
            return file
        }

        /// Every path below the unit, hidden ones too, sorted.
        func listing() throws -> [String] {
            (FileManager.default.subpaths(atPath: url.path) ?? []).sorted()
        }
    }

    static let set: [(String, Data)] = [
        ("base.apk", SplitAPK.make()), ("split_config.de.apk", SplitAPK.make(split: "config.de")),
        ("split_config.xxhdpi.apk", SplitAPK.make(split: "config.xxhdpi")),
    ]

    static func open(_ unit: Unit, _ entries: [(String, Data)], method: ZipBuilder.Method = .deflated) throws -> Container {
        try Container.open(unit.file(ContainerTests.zip(entries, method: method)))
    }

    /// `output`'s entry `name` with `patch` applied to its local header and its central directory record, each given
    /// the record's start and the offset of the field in a local header (the central record's is 2 bytes further).
    static func patching(_ name: String, in output: ZipBuilder.Output, _ patch: (inout Data, _ local: Int, _ central: Int) -> Void)
        throws -> Data {
        var data = output.data
        let local = try #require(output.dataOffsets[name]) - 30 - name.utf8.count
        var search = output.centralDirectoryOffset
        while true {
            let record = try #require(data.range(of: Data([0x50, 0x4B, 0x01, 0x02]), in: search..<data.count)).lowerBound
            let nameLength = Int(data[record + 28]) | Int(data[record + 29]) << 8
            if data[(record + 46)..<(record + 46 + nameLength)] == Data(name.utf8) {
                patch(&data, local, record)
                return data
            }
            search = record + 4
        }
    }

    static func put(_ value: UInt32, at offset: Int, in data: inout Data) {
        withUnsafeBytes(of: value.littleEndian) { data.replaceSubrange(offset..<offset + 4, with: $0) }
    }

    // MARK: - Loose sets (decision 3)

    @Test(arguments: ["SAI", "App Manager", "a zipped folder"])
    func eachLooseSetGivesItsSet(_ layout: String) throws {
        var entries = Self.set
        switch layout {
        case "SAI": entries += [("icon.png", Data([0x89])), ("meta.sai_v2.json", Data("{}".utf8))]
        case "App Manager": entries.insert(("info.json", Data(#"{"info_version": 1}"#.utf8)), at: 0)
        default: entries = entries.map { ("MyApp/\($0.0)", $0.1) } + [("__MACOSX/MyApp/._base.apk", Data([0]))]
        }
        let unit = try Unit()
        let container = try Self.open(unit, entries)
        let files = try container.extract(container.looseSetItems(), into: unit.destination)

        #expect(files.map(\.lastPathComponent) == ["1.apk", "2.apk", "3.apk"])
        #expect(try files.map { try Data(contentsOf: $0) } == Self.set.map(\.1))
        let members = try files.map { try SetMember.read(ZipArchive(url: $0), fileName: $0.lastPathComponent, sha256: "") }
        let checked = try SetChecks.check(members).get()
        #expect(checked.base?.fileName == "1.apk")
        #expect(checked.splits.map(\.splitName) == ["config.de", "config.xxhdpi"])
    }

    // MARK: - The checks

    @Test(arguments: [false, true])
    func anEntryOfSeveralChunksKeepsItsContentAndChecksum(deflated: Bool) throws {
        // More than the reader's 1 MiB chunks, so the checksum runs over all of them.
        var random = SplitMix64(seed: 7)
        let content = Data((0..<(3 << 20) + 7).map { _ in UInt8(truncatingIfNeeded: random.next()) })
        let unit = try Unit()
        let container = try Self.open(unit, [("base.apk", content)], method: deflated ? .deflated : .stored)
        let files = try container.extract(container.looseSetItems(), into: unit.destination)

        #expect(try Data(contentsOf: files[0]) == content)
    }

    @Test func theRunningChecksumSkipsAnEmptyChunk() {
        // zlib's crc32 given no buffer answers its initial value, which would start the sum again (the lead's review
        // of a1). Foundation gives an empty Data a pointer today, so the empty buffer here has none. "123456789" is
        // CRC-32's check value.
        var crc = Data("123".utf8).withUnsafeBytes { Container.checksum(0, adding: $0) }
        crc = Container.checksum(crc, adding: UnsafeRawBufferPointer(start: nil, count: 0))
        crc = Data("456789".utf8).withUnsafeBytes { Container.checksum(crc, adding: $0) }
        #expect(crc == 0xCBF4_3926)
    }

    @Test func aCRCMismatchFailsAndLeavesNothing() throws {
        // Both headers agree on a wrong CRC-32, so the reader passes it and only the content's checksum shows it.
        // Android checks none (zip_archive.cc:71, kCrcChecksEnabled = false); the container's check is Minato's.
        var zip = ZipBuilder()
        for (name, content) in Self.set { zip.add(name, content, method: .deflated) }
        let data = try Self.patching("split_config.de.apk", in: zip.build()) { data, local, central in
            Self.put(0x1234_5678, at: local + 14, in: &data)
            Self.put(0x1234_5678, at: central + 16, in: &data)
        }
        let unit = try Unit()
        let container = try Container.open(unit.file(data))

        #expect(throws: ContainerError.checksumMismatch("split_config.de.apk")) {
            try container.extract(container.looseSetItems(), into: unit.destination)
        }
        #expect(try unit.listing() == ["game.apks"])
    }

    @Test(arguments: [UInt32(1024), UInt32(16 << 20) + 1])
    func aSizeOtherThanDeclaredFailsWithinTheDeclaredSize(_ declared: UInt32) throws {
        // A deflate bomb declares less than it inflates to and is refused at its first chunk past that; one that
        // declares more ends short. Neither leaves a file.
        var zip = ZipBuilder()
        zip.add("base.apk", Data(count: 16 << 20), method: .deflated)
        let data = try Self.patching("base.apk", in: zip.build()) { data, local, central in
            Self.put(declared, at: local + 22, in: &data)
            Self.put(declared, at: central + 24, in: &data)
        }
        let unit = try Unit()
        let container = try Container.open(unit.file(data))

        let expected: APKError = declared < 16 << 20
            ? .truncated("base.apk: more data than its size of \(declared) bytes")
            : .truncated("base.apk: got \(16 << 20) of \(declared) bytes")
        #expect(throws: ContainerError.unreadable(expected)) {
            try container.extract(container.looseSetItems(), into: unit.destination)
        }
        #expect(try unit.listing() == ["game.apks"])
    }

    @Test func tooLittleSpaceIsRefusedBeforeAnyWrite() throws {
        // The declared sizes and a tenth more [guess].
        let unit = try Unit()
        let container = try Self.open(unit, Self.set)
        let declared = UInt64(Self.set.map(\.1.count).reduce(0, +))
        let needed = declared + declared / 10

        #expect(throws: ContainerError.notEnoughSpace(needed: needed, available: needed - 1)) {
            try container.extract(container.looseSetItems(), into: unit.destination, availableSpace: { _ in needed - 1 })
        }
        #expect(try unit.listing() == ["game.apks"])
        var asked: URL?
        let files = try container.extract(container.looseSetItems(), into: unit.destination) { folder in
            asked = folder
            return needed
        }
        #expect(files.count == 3)
        #expect(asked?.standardizedFileURL == unit.url.standardizedFileURL)
    }

    @Test func declaredSizesThatOverflowNeedAllTheSpace() {
        #expect(Container.neededSpace(forSizes: [100, 20]) == 132)
        #expect(Container.neededSpace(forSizes: []) == 0)
        #expect(Container.neededSpace(forSizes: [.max / 2, .max / 2 + 2]) == .max)
        #expect(Container.neededSpace(forSizes: [.max - 5]) == .max)
    }

    @Test func namesAreOursNeverAnEntrysPath() throws {
        // libziparchive takes such names (entry_name_utils-inl.h:28-63); they never become a path.
        let apk = SplitAPK.make()
        let entries = [("../evil.apk", apk), ("/tmp/evil.apk", apk), ("a/../../evil.apk", apk), ("dir/sub/x.apk", apk)]
        let unit = try Unit()
        let container = try Self.open(unit, entries)
        let files = try container.extract(container.looseSetItems(), into: unit.destination)

        #expect(files.map(\.lastPathComponent) == ["1.apk", "2.apk", "3.apk", "4.apk"])
        #expect(try unit.listing() == ["contents", "contents/1.apk", "contents/2.apk", "contents/3.apk", "contents/4.apk",
                                       "game.apks"])
        #expect(!FileManager.default.fileExists(atPath: unit.url.deletingLastPathComponent().appendingPathComponent("evil.apk").path))
    }

    @Test(arguments: ["", ".hidden.apk", "a/b.apk", "..", ".", "x\u{0}.apk", "1.apk", String(repeating: "a", count: 252) + ".apk"])
    func aNameThatIsNoPlainFileNameOrTakenIsRefused(_ name: String) throws {
        let unit = try Unit()
        let container = try Self.open(unit, Self.set)
        let entry = try #require(container.looseSetItems().first?.entry)
        let items = [Container.Item(entry: entry, fileName: "1.apk"), Container.Item(entry: entry, fileName: name)]

        #expect(throws: ContainerError.unsafeName(name)) { try container.extract(items, into: unit.destination) }
        #expect(try unit.listing() == ["game.apks"])
    }

    // MARK: - The folder, whole or not at all

    @Test func aFailureAfterAFileLeavesNothing() throws {
        // The first APK is written, the second fails: the partial folder goes with both.
        var zip = ZipBuilder()
        for (name, content) in Self.set { zip.add(name, content, method: .stored) }
        let data = try Self.patching("split_config.xxhdpi.apk", in: zip.build()) { data, local, central in
            Self.put(0, at: local + 14, in: &data)
            Self.put(0, at: central + 16, in: &data)
        }
        let unit = try Unit()
        let container = try Container.open(unit.file(data))

        #expect(throws: ContainerError.checksumMismatch("split_config.xxhdpi.apk")) {
            try container.extract(container.looseSetItems(), into: unit.destination)
        }
        #expect(try unit.listing() == ["game.apks"])
    }

    @Test func aPartialFolderLeftByAnEarlierRunIsReplaced() throws {
        // After a suspension the unit's extraction starts again.
        let unit = try Unit()
        let partial = unit.url.appendingPathComponent("contents.partial", isDirectory: true)
        try FileManager.default.createDirectory(at: partial, withIntermediateDirectories: true)
        try Data("old".utf8).write(to: partial.appendingPathComponent("1.apk"))
        try Data("old".utf8).write(to: partial.appendingPathComponent("stale.apk"))
        let container = try Self.open(unit, Self.set)
        _ = try container.extract(container.looseSetItems(), into: unit.destination)

        #expect(try unit.listing() == ["contents", "contents/1.apk", "contents/2.apk", "contents/3.apk", "game.apks"])
        #expect(try Data(contentsOf: unit.destination.appendingPathComponent("1.apk")) == Self.set[0].1)
    }

    @Test func anExistingFolderIsNeverReplaced() throws {
        let unit = try Unit()
        try FileManager.default.createDirectory(at: unit.destination, withIntermediateDirectories: true)
        try Data("mine".utf8).write(to: unit.destination.appendingPathComponent("1.apk"))
        let container = try Self.open(unit, Self.set)

        #expect(throws: ContainerError.destinationExists) {
            try container.extract(container.looseSetItems(), into: unit.destination)
        }
        #expect(try Data(contentsOf: unit.destination.appendingPathComponent("1.apk")) == Data("mine".utf8))
    }

    @Test func theFolderIsExcludedFromBackups() throws {
        // Extraction temporaries stay out of backups (the M5 plan's decision 9).
        let unit = try Unit()
        let container = try Self.open(unit, Self.set)
        _ = try container.extract(container.looseSetItems(), into: unit.destination)

        #expect(try unit.destination.resourceValues(forKeys: [.isExcludedFromBackupKey]).isExcludedFromBackup == true)
    }
}
