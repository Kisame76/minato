import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

/// What a picked file holds, told by its content in InstallerX's order (ADR-0049 decision 1), its caps (decision 7)
/// and the APKs of a loose set (decision 3).
@Suite("Containers")
struct ContainerTests {
    /// A file in a folder of its own, removed with the test.
    final class File {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("container-\(UUID().uuidString)",
                                                                                   isDirectory: true)
        let url: URL

        init(_ data: Data, name: String) throws {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            url = folder.appendingPathComponent(name)
            try data.write(to: url)
        }

        deinit { try? FileManager.default.removeItem(at: folder) }
    }

    static func zip(_ entries: [(String, Data)], method: ZipBuilder.Method = .stored) -> Data {
        var zip = ZipBuilder()
        for (name, content) in entries { zip.add(name, content, method: method) }
        return zip.build().data
    }

    static func kind(_ entries: [(String, Data)], name: String) throws -> ContainerKind {
        let file = try File(zip(entries), name: name)
        return try Container.open(file.url).kind
    }

    static let apk = Data("not parsed here".utf8)
    static func json(_ text: String) -> Data { Data(text.utf8) }

    // MARK: - Detection

    @Test func eachKindIsToldByItsContentUnderAnotherExtension() throws {
        // APKCombo serves XAPK content as .apks; the extension decides nothing.
        let xapk = [("manifest.json", Self.json(#"{"xapk_version": 2, "package_name": "org.example"}"#)),
                    ("org.example.apk", Self.apk)]
        #expect(try Self.kind(xapk, name: "game.apks") == .xapk)
        let apkm = [("info.json", Self.json(#"{"apkm_version": 5, "pname": "org.example"}"#)), ("base.apk", Self.apk)]
        #expect(try Self.kind(apkm, name: "game.zip") == .apkm)
        // A plain APKM's info.json with only its package name counts too.
        #expect(try Self.kind([("info.json", Self.json(#"{"pname": "org.example"}"#)), ("base.apk", Self.apk)],
                              name: "game.apks") == .apkm)
        #expect(try Self.kind([("AndroidManifest.xml", Self.apk), ("classes.dex", Self.apk)], name: "game.xapk") == .apk)
        #expect(try Self.kind([("toc.pb", Data([0x0A, 0x00])), ("splits/base-master.apk", Self.apk)],
                              name: "game.xapk") == .bundletool)
        #expect(try Self.kind([("toc.json", Self.json("{}")), ("universal.apk", Self.apk)], name: "game.zip") == .bundletool)
        // SAI's, App Manager's and a plain ZIP are loose sets.
        let sai = [("base.apk", Self.apk), ("split_config.de.apk", Self.apk), ("icon.png", Self.apk),
                   ("meta.sai_v2.json", Self.json(#"{"package": "org.example"}"#))]
        #expect(try Self.kind(sai, name: "game.xapk") == .looseSet)
        let appManager = [("info.json", Self.json(#"{"info_version": 1, "package_name": "org.example"}"#)),
                          ("base.apk", Self.apk)]
        #expect(try Self.kind(appManager, name: "game.apkm") == .looseSet)
        #expect(try Self.kind([("base.apk", Self.apk)], name: "game.apkm") == .looseSet)
    }

    @Test func theFirstMatchInInstallerXsOrderDecides() throws {
        // FileTypeDetector.kt:192-279: manifest.json, then info.json with APKM's keys, then AndroidManifest.xml, then
        // a toc; only the root counts.
        let apkmInfo = Self.json(#"{"apkm_version": 5}"#)
        #expect(try Self.kind([("info.json", apkmInfo), ("manifest.json", Self.json("{}"))], name: "a.zip") == .xapk)
        #expect(try Self.kind([("AndroidManifest.xml", Self.apk), ("info.json", apkmInfo)], name: "a.zip") == .apkm)
        #expect(try Self.kind([("toc.pb", Data()), ("AndroidManifest.xml", Self.apk)], name: "a.zip") == .apk)
        #expect(try Self.kind([("base.apk", Self.apk), ("toc.pb", Data())], name: "a.zip") == .bundletool)
        #expect(try Self.kind([("sub/manifest.json", Self.json("{}")), ("sub/AndroidManifest.xml", Self.apk),
                               ("base.apk", Self.apk)], name: "a.zip") == .looseSet)
    }

    @Test(arguments: [#"{"name": "My App", "start_url": "/"}"#, "{", #"["xapk_version"]"#, "{}"])
    func anAPKWithARootManifestJsonOfItsOwnIsAnAPK(_ text: String) throws {
        // A departure from InstallerX's order (ADR-0049 c1): beside AndroidManifest.xml, a manifest.json with neither
        // of XAPK's keys, xapk_version and package_name, is the APK's own file, as a web app's.
        let entries = [("AndroidManifest.xml", Self.apk), ("manifest.json", Self.json(text)), ("classes.dex", Self.apk)]
        #expect(try Self.kind(entries, name: "app.apk") == .apk)
        // Without AndroidManifest.xml it is still an XAPK, whose files then count by themselves.
        #expect(try Self.kind([("manifest.json", Self.json(text)), ("base.apk", Self.apk)], name: "a.xapk") == .xapk)
    }

    @Test(arguments: [#"{"xapk_version": 2}"#, #"{"package_name": "org.example"}"#, #"{"xapk_version": null}"#])
    func eitherOfXAPKsKeysMakesAnXAPKBesideAnAndroidManifest(_ text: String) throws {
        let entries = [("AndroidManifest.xml", Self.apk), ("manifest.json", Self.json(text)), ("org.example.apk", Self.apk)]
        #expect(try Self.kind(entries, name: "a.apk") == .xapk)
    }

    @Test func aManifestJsonBesideAnAndroidManifestIsReadOnlyWithinItsCapAndNeverEncrypted() throws {
        // Over 1 MiB or encrypted, it is not read and so is no XAPK's: Android never reads it either.
        let big = Data(#"{"xapk_version": 2, "name": ""#.utf8) + Data(repeating: 0x61, count: Int(Container.maxJSONSize))
            + Data(#""}"#.utf8)
        let over = try File(Self.zip([("AndroidManifest.xml", Self.apk), ("manifest.json", big)], method: .deflated), name: "a.apk")
        #expect(try Container.open(over.url).kind == .apk)
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", Self.apk, method: .stored)
        zip.add("manifest.json", Self.json(#"{"xapk_version": 2}"#), method: .stored)
        let encrypted = try ContainerExtractionTests.patching("manifest.json", in: zip.build()) { data, local, central in
            data[local + 6] |= 0x01
            data[central + 8] |= 0x01
        }
        let file = try File(encrypted, name: "a.apk")
        #expect(try Container.open(file.url).kind == .apk)
        // Without AndroidManifest.xml, an encrypted manifest.json refuses the container, as any encrypted entry does.
        var alone = ZipBuilder()
        alone.add("manifest.json", Self.json(#"{"xapk_version": 2}"#), method: .stored)
        alone.add("base.apk", Self.apk, method: .stored)
        let refused = try File(try ContainerExtractionTests.patching("manifest.json", in: alone.build()) { data, local, central in
            data[local + 6] |= 0x01
            data[central + 8] |= 0x01
        }, name: "a.xapk")
        #expect(throws: ContainerError.encrypted("manifest.json")) { try Container.open(refused.url) }
    }

    @Test(arguments: ["{", #"["apkm_version"]"#, #"{"versioncode": "5"}"#, #"{"info_version": 1}"#])
    func anInfoJsonWithoutAPKMsKeysIsNoAPKM(_ text: String) throws {
        // Unreadable JSON, another shape or neither key: not an APKM, so the APKs make a loose set.
        #expect(try Self.kind([("info.json", Self.json(text)), ("base.apk", Self.apk)], name: "a.apkm") == .looseSet)
    }

    // MARK: - Refusals

    @Test func aFileThatIsNoZipIsRefusedAndAnEncryptedAPKMNamed() throws {
        // UnApkm.java:190-198: an encrypted APKM is no ZIP, told by its first 4 bytes.
        let encrypted = Data([0x00, 0x08, 0x00, 0x00]) + Data(repeating: 0x5A, count: 64)
        for (name, apkm) in [("game.apkm", true), ("GAME.APKM", true), ("game.apks", false), ("game", false)] {
            let file = try File(encrypted, name: name)
            #expect(throws: ContainerError.notAZip(encryptedAPKM: apkm)) { try Container.open(file.url) }
        }
        #expect(ContainerError.notAZip(encryptedAPKM: true).errorDescription == "Encrypted APKM files aren’t supported")
        // Shorter than a signature.
        let short = try File(Data([0x50, 0x4B]), name: "a.zip")
        #expect(throws: ContainerError.notAZip(encryptedAPKM: false)) { try Container.open(short.url) }
    }

    @Test func anEmptyZipIsRefusedAsTheReaderRefusesIt() throws {
        // PK\5\6 is a ZIP's signature, and libziparchive refuses it as empty (zip_archive.cc:409-414).
        let empty = Data([0x50, 0x4B, 0x05, 0x06]) + Data(count: 18)
        let file = try File(empty, name: "a.apks")
        #expect(throws: ContainerError.unreadable(.invalidArchive("it holds no entries"))) { try Container.open(file.url) }
    }

    @Test func anEncryptedEntryRefusesTheContainer() throws {
        // Any entry with the ZIP encryption flag (bit 0 of its general purpose flags); Minato never decrypts.
        let output = { () -> ZipBuilder.Output in
            var zip = ZipBuilder()
            zip.add("base.apk", Self.apk, method: .stored)
            zip.add("split_config.de.apk", Self.apk, method: .stored)
            return zip.build()
        }()
        var data = output.data
        let second = try #require(output.dataOffsets["split_config.de.apk"])
        let localHeader = second - 30 - "split_config.de.apk".utf8.count
        data[localHeader + 6] |= 0x01
        let directory = try #require(data.range(of: Data([0x50, 0x4B, 0x01, 0x02]),
                                                in: (output.centralDirectoryOffset + 4)..<data.count))
        data[directory.lowerBound + 8] |= 0x01
        let file = try File(data, name: "a.apks")
        #expect(throws: ContainerError.encrypted("split_config.de.apk")) { try Container.open(file.url) }
    }

    @Test func anEncryptedInfoJsonIsNeverRead() throws {
        var zip = ZipBuilder()
        zip.add("info.json", Self.json(#"{"pname": "org.example"}"#), method: .stored)
        zip.add("base.apk", Self.apk, method: .stored)
        let data = try ContainerExtractionTests.patching("info.json", in: zip.build()) { data, local, central in
            data[local + 6] |= 0x01
            data[central + 8] |= 0x01
        }
        let file = try File(data, name: "a.apkm")
        #expect(throws: ContainerError.encrypted("info.json")) { try Container.open(file.url) }
    }

    // MARK: - Caps (decision 7)

    @Test func aContainerOverItsEntriesIsRefusedWhereAnAPKIsNot() throws {
        let many = (0..<Container.maxEntries).map { ("assets/\($0)", Data()) }
        let file = try File(Self.zip(many + [("base.apk", Self.apk)]), name: "a.apks")
        #expect(throws: ContainerError.overCap("it holds 4097 entries, more than the 4096 Minato takes from a container")) {
            try Container.open(file.url)
        }
        // An APK keeps the reader's own cap, far above.
        let apk = try File(Self.zip([("AndroidManifest.xml", Self.apk)] + many), name: "a.apk")
        #expect(try Container.open(apk.url).kind == .apk)
        // The cap itself passes.
        let full = try File(Self.zip(Array(many.dropLast()) + [("base.apk", Self.apk)]), name: "a.apks")
        #expect(try Container.open(full.url).archive.entries.count == Container.maxEntries)
    }

    @Test func aLooseSetOverItsAPKsIsRefused() throws {
        let apks = (0..<Container.maxAPKs + 1).map { ("split_\($0).apk", Self.apk) }
        let file = try File(Self.zip(apks), name: "a.zip")
        let container = try Container.open(file.url)
        #expect(throws: ContainerError.overCap("it holds 513 APKs, more than the 512 Minato takes from a container")) {
            try container.looseSetAPKs()
        }
        let full = try File(Self.zip(Array(apks.dropLast())), name: "a.zip")
        #expect(try Container.open(full.url).looseSetAPKs().count == Container.maxAPKs)
    }

    @Test func aSpanningMarkerIsAZipSignatureThatTheReaderJudges() throws {
        // PK\7\8 counts as a ZIP's start for the kind (App Manager's FileUtils.java:50-58); libziparchive refuses a
        // file that does not start with a local header (zip_archive.cc:649-663), so it is damaged, not "no ZIP".
        let file = try File(Data([0x50, 0x4B, 0x07, 0x08]) + Self.zip([("base.apk", Self.apk)]), name: "a.apkm")
        #expect {
            try Container.open(file.url)
        } throws: { error in
            if case .unreadable = error as? ContainerError { true } else { false }
        }
    }

    @Test func anInfoJsonOverItsCapIsRefused() throws {
        let big = Data(#"{"pname": ""#.utf8) + Data(repeating: 0x61, count: Int(Container.maxJSONSize)) + Data(#""}"#.utf8)
        let file = try File(Self.zip([("info.json", big), ("base.apk", Self.apk)], method: .deflated), name: "a.apkm")
        #expect(throws: ContainerError.overCap("its info.json has \(big.count) bytes, more than the 1048576 Minato reads")) {
            try Container.open(file.url)
        }
    }

    // MARK: - A loose set's APKs (decision 3)

    @Test func aLooseSetTakesEveryAPKButHiddenFilesAndMacOSsCopies() throws {
        // Every .apk entry, in any folder and in any case; the extension's dot needs a name before it. Hidden files
        // and the AppleDouble copies macOS's Archive Utility adds under __MACOSX/ are no APKs, nor are folders.
        let entries = [("base.apk", Self.apk), ("icon.png", Self.apk), ("meta.sai_v2.json", Self.json("{}")),
                       ("app/split_config.de.apk", Self.apk), ("UPPER.APK", Self.apk), (".hidden.apk", Self.apk),
                       ("app/._base.apk", Self.apk), ("__MACOSX/app/._base.apk", Self.apk), ("__MACOSX/x.apk", Self.apk),
                       ("folder.apk/", Data()), (".apk", Self.apk), ("readme.apk.txt", Self.apk)]
        let file = try File(Self.zip(entries), name: "a.zip")
        let container = try Container.open(file.url)
        #expect(container.kind == .looseSet)
        #expect(try container.looseSetAPKs().map(\.name) == ["base.apk", "app/split_config.de.apk", "UPPER.APK"])
    }

    @Test func aLooseSetWithoutAPKsIsRefused() throws {
        let file = try File(Self.zip([("readme.txt", Self.apk), ("__MACOSX/._base.apk", Self.apk)]), name: "a.zip")
        #expect(throws: ContainerError.noAPK) { try Container.open(file.url).looseSetAPKs() }
    }

    // MARK: - What the library shows (ADR-0049 a3)

    @Test(arguments: [
        (ContainerError.notAZip(encryptedAPKM: true), "Encrypted APKM files aren’t supported"),
        (.notAZip(encryptedAPKM: false), "It is neither an APK nor a container of APKs"),
        (.unreadable(.invalidArchive("it holds no entries")), "It is damaged: it holds no entries"),
        (.unreadable(.truncated("base.apk: got 3 of 9 bytes")), "It is damaged: base.apk: got 3 of 9 bytes"),
        (.unreadable(.notAZipArchive), "It is damaged: it is no ZIP archive"),
        (.unreadable(.crcMismatch("base.apk")), "It is damaged: base.apk does not match its checksum"),
        (.encrypted("base.apk"), "“base.apk” in it is encrypted, and Minato never decrypts"),
        (.overCap("it holds 513 APKs, more than the 512 Minato takes from a container"),
         "It holds 513 APKs, more than the 512 Minato takes from a container"),
        (.noAPK, "It holds no APK"),
        (.checksumMismatch("base.apk"), "“base.apk” in it is damaged: its checksum does not match"),
        (.notEnoughSpace(needed: 3_000_000, available: 1_000_000), "It needs 3 MB of free space, and 1 MB are free"),
        (.invalidTOC("its toc.pb is cut short in a varint"), "Its toc.pb is cut short in a varint"),
        (.incompatible("No arm64-v8a APK"), "No arm64-v8a APK"),
    ] as [(ContainerError, String)])
    func eachRefusalIsASentenceForTheLibrary(_ error: ContainerError, _ refusal: String) {
        // The library shows an entry's refusal as it is, after "Not installed:".
        #expect(error.refusal == refusal)
    }
}
