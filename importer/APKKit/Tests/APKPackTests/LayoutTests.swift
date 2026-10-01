import APKKit
import APKPack
import APKTestSupport
import Foundation
import Testing

@Suite("APKLayout")
struct LayoutTests {
    static func manifest(extractNativeLibs: String? = "false", libraryName: String = "activity-test",
                         hasCode: String? = "false") throws -> Data {
        var text = ManifestCompilerTests.nativeActivity
            .replacingOccurrences(of: #"android:value="activity-test""#, with: #"android:value="\#(libraryName)""#)
        let attribute = #" android:extractNativeLibs="false""#
        text = text.replacingOccurrences(of: attribute, with: extractNativeLibs.map { #" android:extractNativeLibs="\#($0)""# } ?? "")
        text = text.replacingOccurrences(of: #" android:hasCode="false""#, with: hasCode.map { #" android:hasCode="\#($0)""# } ?? "")
        return try ManifestCompiler.compile(Data(text.utf8), file: "AndroidManifest.xml")
    }

    /// A DEX file as far as make-apk reads it: the magic "dex\n", a version of three digits and a NUL.
    static func dex(_ fill: UInt8 = 0x44, version: String = "035") -> Data {
        Data("dex\n\(version)\0".utf8) + Data(repeating: fill, count: 300)
    }

    static let nativeActivityLibrary = PackFile(name: "libactivity-test.so", contents: library())

    /// The start of an AArch64 shared library (e_machine 183), as ELFProbe reads it.
    static func library(machine: UInt16 = 183) -> Data {
        var elf = ELFBuilder(segments: [.load(align: 0x4000), .load(align: 0x4000)])
        elf.machine = machine
        return elf.build() + Data(repeating: 0x11, count: 3_000)
    }

    static let assets = [
        PackFile(name: "z.ogg", contents: Data(repeating: 0x4F, count: 300)),
        PackFile(name: "dir/b.png", contents: Data(repeating: 0x50, count: 300)),
        PackFile(name: "a.txt", contents: Data(repeating: 0x61, count: 300)),
        PackFile(name: "B.txt", contents: Data(repeating: 0x42, count: 300)),
    ]

    static func pack(extractNativeLibs: String? = "false") throws -> Data {
        try APKLayout.build(
            manifest: manifest(extractNativeLibs: extractNativeLibs),
            libraries: [PackFile(name: "libzzz.so", contents: library()), PackFile(name: "libactivity-test.so", contents: library())],
            assets: assets)
    }

    static func u16(_ data: Data, _ offset: UInt64) -> UInt16 {
        let start = data.startIndex + Int(offset)
        return UInt16(data[start]) | UInt16(data[start + 1]) << 8
    }

    @Test func laysOutEntriesAsAapt2AndZipalignDo() throws {
        let archive = try ZipArchive(data: Self.pack())

        #expect(archive.entries.map(\.name) == [
            "AndroidManifest.xml",
            "lib/arm64-v8a/libactivity-test.so",
            "lib/arm64-v8a/libzzz.so",
            "assets/B.txt",
            "assets/a.txt",
            "assets/dir/b.png",
            "assets/z.ogg",
        ])
        let methods = Dictionary(uniqueKeysWithValues: archive.entries.map { ($0.name, $0.method) })
        #expect(methods["AndroidManifest.xml"] == .deflated)
        #expect(methods["lib/arm64-v8a/libactivity-test.so"] == .stored)
        #expect(methods["lib/arm64-v8a/libzzz.so"] == .stored)
        #expect(methods["assets/a.txt"] == .deflated)
        #expect(methods["assets/dir/b.png"] == .stored)
        #expect(methods["assets/z.ogg"] == .stored)
        for entry in archive.entries where entry.name.hasPrefix("lib/") {
            #expect(try archive.dataOffset(of: entry) % 16_384 == 0, "\(entry.name)")
        }
        for entry in archive.entries where entry.method == .stored {
            #expect(try archive.dataOffset(of: entry) % 4 == 0, "\(entry.name)")
        }
        #expect(try archive.data(named: "assets/dir/b.png") == Data(repeating: 0x50, count: 300))
        #expect(try archive.data(named: "assets/a.txt") == Data(repeating: 0x61, count: 300))
    }

    @Test(arguments: [nil, "true"] as [String?])
    func deflatesLibrariesThatAndroidExtracts(extractNativeLibs: String?) throws {
        let archive = try ZipArchive(data: Self.pack(extractNativeLibs: extractNativeLibs))

        #expect(archive.entry(named: "lib/arm64-v8a/libactivity-test.so")?.method == .deflated)
        #expect(try archive.data(named: "lib/arm64-v8a/libactivity-test.so") == Self.library())
    }

    @Test func deflatesLargeDataThatDoesNotCompress() throws {
        // Random bytes grow when deflated: Apple's zlib stores them in blocks with a few bytes each.
        var generator = SplitMix64(seed: 31)
        var noise = Data(count: 4 << 20)
        noise.withUnsafeMutableBytes { raw in
            for word in 0..<raw.count / 8 {
                raw.storeBytes(of: generator.next(), toByteOffset: 8 * word, as: UInt64.self)
            }
        }

        let archive = try ZipArchive(data: APKLayout.build(
            manifest: Self.manifest(), libraries: [PackFile(name: "libactivity-test.so", contents: Self.library())],
            assets: [PackFile(name: "texture.bin", contents: noise)]))

        #expect(archive.entry(named: "assets/texture.bin")?.method == .deflated)
        #expect(try archive.data(named: "assets/texture.bin") == noise)
    }

    @Test func givesEveryEntryTheSameFixedTime() throws {
        let data = try Self.pack()
        let archive = try ZipArchive(data: data)

        for entry in archive.entries {
            // Local header: time at offset 10, date at 12 (MS-DOS format): 1980-01-01 00:00.
            #expect(Self.u16(data, entry.localHeaderOffset + 10) == 0x0000, "\(entry.name)")
            #expect(Self.u16(data, entry.localHeaderOffset + 12) == 0x0021, "\(entry.name)")
        }
    }

    @Test func packsTheSameInputsToTheSameBytes() throws {
        let first = try Self.pack()

        #expect(try Self.pack() == first)
        #expect(try ZipArchive(data: first).entries.count == 7)
    }

    @Test func readsBackAsTheAppReadsIt() throws {
        let report = try APKAnalyzer.analyze(ZipArchive(data: Self.pack()), preferredLanguages: ["en"])

        #expect(report.compatibility.status == .ready)
        #expect(report.runtime == .nativeActivity(library: "activity-test"))
        #expect(report.abis == ["arm64-v8a"])
        #expect(report.selectedABI == "arm64-v8a")
        #expect(report.nativeLibraries.map(\.name) == ["libactivity-test.so", "libzzz.so"])
        #expect(report.nativeLibraries.allSatisfy { !$0.isCompressed && $0.isZipAligned16K == true })
        #expect(report.label == "Activity Test")
        #expect(report.versionCode == 7)
        #expect(report.minSdk == 29)
        #expect(report.targetSdk == 35)
        #expect(report.extractNativeLibs == false)
        #expect(report.requiredGLESVersion == 0x0003_0000)
        #expect(report.requiredVulkanVersion == 0x0040_1000)
    }

    /// Android's installer skips these (ApkParsing.cpp): no lib prefix, no .so suffix, and characters
    /// outside A-Z, a-z, 0-9 and + , - . = _.
    @Test(arguments: ["game.so", "libgame.so.1", "lib foo.so", "lib@x.so", "libcafé.so"])
    func refusesLibraryNamesAndroidSkips(name: String) throws {
        let error = try #require(throws: PackError.self) {
            try APKLayout.build(manifest: Self.manifest(),
                                libraries: [PackFile(name: "libactivity-test.so", contents: Self.library()),
                                            PackFile(name: name, contents: Self.library())],
                                assets: [])
        }

        #expect(error.file == name)
    }

    @Test(arguments: [LayoutTests.library(machine: 62), Data("not an ELF file".utf8)])
    func refusesLibrariesThatAreNotAArch64(contents: Data) throws {
        let error = try #require(throws: PackError.self) {
            try APKLayout.build(manifest: Self.manifest(),
                                libraries: [PackFile(name: "libactivity-test.so", contents: contents)],
                                assets: [])
        }

        #expect(error.file == "libactivity-test.so")
    }

    @Test func refusesANativeActivityWithoutItsLibrary() throws {
        let error = try #require(throws: PackError.self) {
            try APKLayout.build(manifest: Self.manifest(),
                                libraries: [PackFile(name: "libother.so", contents: Self.library())],
                                assets: [])
        }

        #expect(error.file == "libactivity-test.so")
    }

    @Test func refusesTwoLibrariesWithTheSameName() throws {
        let error = try #require(throws: PackError.self) {
            try APKLayout.build(manifest: Self.manifest(),
                                libraries: [PackFile(name: "libactivity-test.so", contents: Self.library()),
                                            PackFile(name: "libactivity-test.so", contents: Self.library())],
                                assets: [])
        }

        #expect(error.file == "libactivity-test.so")
    }

    @Test func acceptsEveryCharacterAndroidAllowsInALibraryName() throws {
        let name = "libAZaz09+,-.=_.so"

        let archive = try ZipArchive(data: APKLayout.build(
            manifest: Self.manifest(),
            libraries: [PackFile(name: "libactivity-test.so", contents: Self.library()), PackFile(name: name, contents: Self.library())],
            assets: []))

        #expect(archive.entry(named: "lib/arm64-v8a/\(name)") != nil)
    }

    @Test func refusesTwoAssetsWithTheSameName() throws {
        // Android's zip reader refuses an APK with two entries of the same name.
        let error = try #require(throws: PackError.self) {
            try APKLayout.build(manifest: Self.manifest(),
                                libraries: [PackFile(name: "libactivity-test.so", contents: Self.library())],
                                assets: [PackFile(name: "a.txt", contents: Data("1".utf8)), PackFile(name: "a.txt", contents: Data("2".utf8))])
        }

        #expect(error.file == "a.txt")
    }

    // MARK: - Java code (ADR-0039, decision 11)

    static func packCode(_ dex: [PackFile], hasCode: String? = nil) throws -> Data {
        try APKLayout.build(manifest: manifest(hasCode: hasCode), dex: dex, libraries: [nativeActivityLibrary],
                            assets: [PackFile(name: "a.txt", contents: Data("a".utf8))])
    }

    /// ART maps a DEX entry in place when it is stored and 4-byte aligned, and extracts it otherwise
    /// (dex_file_loader.cc); its names run classes.dex, classes2.dex, … in that order.
    @Test func storesDexFilesAlignedRightAfterTheManifest() throws {
        let archive = try ZipArchive(data: Self.packCode([
            PackFile(name: "classes10.dex", contents: Self.dex(0x0A)),
            PackFile(name: "classes2.dex", contents: Self.dex(0x02)),
            PackFile(name: "classes.dex", contents: Self.dex(0x01)),
        ] + (3...9).map { PackFile(name: "classes\($0).dex", contents: Self.dex(UInt8($0))) }))

        #expect(archive.entries.map(\.name) == ["AndroidManifest.xml", "classes.dex"]
            + (2...10).map { "classes\($0).dex" } + ["lib/arm64-v8a/libactivity-test.so", "assets/a.txt"])
        for entry in archive.entries where entry.name.hasSuffix(".dex") {
            #expect(entry.method == .stored, "\(entry.name)")
            #expect(try archive.dataOffset(of: entry) % 4 == 0, "\(entry.name)")
        }
        #expect(try archive.data(named: "classes.dex") == Self.dex(0x01))
        #expect(try archive.data(named: "classes10.dex") == Self.dex(0x0A))
    }

    @Test(arguments: [nil, "true"] as [String?])
    func readsBackAsAnAppWithJavaCode(hasCode: String?) throws {
        let report = try APKAnalyzer.analyze(
            ZipArchive(data: Self.packCode([PackFile(name: "classes.dex", contents: Self.dex())], hasCode: hasCode)),
            preferredLanguages: ["en"])

        #expect(report.compatibility.issues.contains(.needsART))
    }

    /// ART opens classes.dex, then classes2.dex and up; it never looks for any other name.
    @Test(arguments: ["dex.dex", "classes1.dex", "classes02.dex", "Classes.dex", "classes.dex.bak", "classes2.DEX"])
    func refusesDexNamesARTNeverOpens(name: String) throws {
        let error = try #require(throws: PackError.self) {
            try Self.packCode([PackFile(name: "classes.dex", contents: Self.dex()), PackFile(name: name, contents: Self.dex())])
        }

        #expect(error.file == name)
        #expect(error.message.contains("classes.dex, classes2.dex"), "\(error.message)")
    }

    /// ART stops at the first missing name, so every DEX file after a gap would never load.
    @Test(arguments: [(["classes.dex", "classes3.dex"], "classes3.dex", "classes2.dex"),
                      (["classes2.dex"], "classes2.dex", "classes.dex")])
    func refusesAGapInTheDexNames(names: [String], file: String, missing: String) throws {
        let error = try #require(throws: PackError.self) {
            try Self.packCode(names.map { PackFile(name: $0, contents: Self.dex()) })
        }

        #expect(error.file == file)
        #expect(error.message.contains("\(missing) is missing"), "\(error.message)")
    }

    @Test func refusesTwoDexFilesWithTheSameName() throws {
        let error = try #require(throws: PackError.self) {
            try Self.packCode([PackFile(name: "classes.dex", contents: Self.dex()),
                               PackFile(name: "classes.dex", contents: Self.dex(0x01))])
        }

        #expect(error.file == "classes.dex")
        #expect(error.message == "given twice")
    }

    @Test(arguments: [Data("not a dex file".utf8), LayoutTests.dex(version: "03x"), Data("dex\n035".utf8),
                      LayoutTests.library()])
    func refusesFilesThatAreNotDex(contents: Data) throws {
        let error = try #require(throws: PackError.self) {
            try Self.packCode([PackFile(name: "classes.dex", contents: contents, origin: "build/classes.dex")])
        }

        #expect(error.file == "build/classes.dex")
        #expect(error.message.hasPrefix("not a DEX file"), "\(error.message)")
    }

    /// Android never loads the code of an app that says it has none.
    @Test func refusesDexWhenTheManifestSaysThereIsNoCode() throws {
        let error = try #require(throws: PackError.self) {
            try Self.packCode([PackFile(name: "classes.dex", contents: Self.dex())], hasCode: "false")
        }

        #expect(error.file == "classes.dex")
        #expect(error.message.contains(#"android:hasCode="false""#), "\(error.message)")
    }

    /// Android 15 refuses to install it: ScanPackageUtils.assertCodePolicy, "code is missing".
    @Test(arguments: [nil, "true"] as [String?])
    func refusesAManifestWithCodeButNoDex(hasCode: String?) throws {
        let error = try #require(throws: PackError.self) { try Self.packCode([], hasCode: hasCode) }

        #expect(error.file == "classes.dex")
        #expect(error.message.contains("code is missing"), "\(error.message)")
    }

    @Test func takesAssetsFromAFolderWithoutHiddenFiles() throws {
        let folder = URL.temporaryDirectory.appending(path: "make-apk-assets-\(UUID().uuidString)", directoryHint: .isDirectory)
        defer { try? FileManager.default.removeItem(at: folder) }
        let files = [
            "a.txt": "a",
            "sub/b.png": "b",
            ".DS_Store": "hidden",
            ".hidden/c.txt": "hidden",
            "sub/.git/d.txt": "hidden",
        ]
        for (path, text) in files {
            let url = folder.appending(path: path)
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data(text.utf8).write(to: url)
        }

        let assets = try PackSources.assets(in: folder)

        #expect(assets == [
            PackFile(name: "a.txt", contents: Data("a".utf8)),
            PackFile(name: "sub/b.png", contents: Data("b".utf8)),
        ])
    }

    /// A link could lead in a circle or nowhere; make-apk packs files, not links.
    @Test(arguments: ["../a.txt", ".", "missing"])
    func refusesSymbolicLinksInTheAssets(destination: String) throws {
        let folder = URL.temporaryDirectory.appending(path: "make-apk-links-\(UUID().uuidString)", directoryHint: .isDirectory)
        defer { try? FileManager.default.removeItem(at: folder) }
        try FileManager.default.createDirectory(at: folder.appending(path: "sub"), withIntermediateDirectories: true)
        try Data("a".utf8).write(to: folder.appending(path: "a.txt"))
        try FileManager.default.createSymbolicLink(atPath: folder.appending(path: "sub/link").path, withDestinationPath: destination)

        let error = try #require(throws: PackError.self) { try PackSources.assets(in: folder) }

        #expect(error.file == "sub/link")
    }
}
