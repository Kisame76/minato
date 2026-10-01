import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKPack

/// make-apk's --resources (ADR-0039 decision 11, part 2): aapt2's linked output, its compiled manifest, its table and
/// its res/ files, packed with our libraries, DEX files and assets and signed.
@Suite("make-apk --resources")
struct LinkedResourcesTests {
    typealias E = AXMLBuilder.Element

    /// aapt2's manifest for a NativeActivity whose label is a string resource.
    static func manifest(label: UInt32, hasCode: Bool = false) -> Data {
        let activity = E("activity", [.android("name", 0x0101_0003, .string("android.app.NativeActivity")),
                                      .android("exported", 0x0101_0010, .bool(true))], [
            E("meta-data", [.android("name", 0x0101_0003, .string("android.app.lib_name")),
                            .android("value", 0x0101_0024, .string("activity-test"))]),
            E("intent-filter", [], [
                E("action", [.android("name", 0x0101_0003, .string("android.intent.action.MAIN"))]),
                E("category", [.android("name", 0x0101_0003, .string("android.intent.category.LAUNCHER"))]),
            ]),
        ])
        let application = E("application", [.android("label", 0x0101_0001, .reference(label)),
                                            .android("hasCode", 0x0101_000C, .bool(hasCode)),
                                            .android("extractNativeLibs", 0x0101_04EA, .bool(false))], [activity])
        let usesSDK = E("uses-sdk", [.android("minSdkVersion", 0x0101_020C, .int(29)),
                                     .android("targetSdkVersion", 0x0101_0270, .int(35))])
        return AXMLBuilder().build(E("manifest", [.plain("package", .string("io.github.kisame76.minato.check.res"))],
                                     [usesSDK, application]))
    }

    /// An APK as aapt2 link writes it: the manifest deflated, res/ files deflated or stored, the table stored and aligned.
    static func linked(extra: [ZipBuilder.Entry] = [], tableMethod: ZipBuilder.Method = .stored, hasCode: Bool = false,
                       withTable: Bool = true, withManifest: Bool = true) -> Data {
        var table = ARSCBuilder()
        let label = table.add("string", "app_name", [(.default, .string("Minato check")),
                                                     (ARSCBuilder.Config(language: "de"), .string("Minato-Prüfung"))])
        var zip = ZipBuilder()
        if withManifest { zip.add("AndroidManifest.xml", manifest(label: label, hasCode: hasCode)) }
        zip.add("res/layout/main.xml", Data("<View/>".utf8))
        zip.add("res/drawable/icon.png", Data(repeating: 0x89, count: 200), method: .stored, alignment: 4)
        if withTable {
            zip.add("resources.arsc", table.build(), method: tableMethod, alignment: tableMethod == .stored ? 4 : nil)
        }
        zip.entries += extra
        return zip.build().data
    }

    static func pack(_ apk: Data = linked(), dex: [PackFile] = [], assets: [PackFile] = []) throws -> Data {
        let resources = try LinkedResources.read(apk, origin: "build/linked.apk")
        return try APKLayout.build(manifest: resources.manifest, dex: dex,
                                   libraries: [LayoutTests.nativeActivityLibrary], assets: assets,
                                   resources: resources)
    }

    // MARK: - The command line

    @Test func readsTheResourcesOption() throws {
        let options = try MakeAPKOptions.parse(["--resources", "build/linked.apk", "--lib", "build/liba.so",
                                                "--out", "a.apk"])

        #expect(options.resources == "build/linked.apk")
        #expect(options.manifest == nil)
    }

    /// One source of the manifest: aapt2's or our compiler's.
    @Test(arguments: [
        ["--resources", "r.apk", "--manifest", "m.xml", "--out", "a.apk"],
        ["--out", "a.apk"],
        ["--resources", "r.apk", "--resources", "s.apk", "--out", "a.apk"],
        ["--resources", "--out", "a.apk"],
    ])
    func refusesAResourcesOptionThatClashesWithStatus64(arguments: [String]) throws {
        let error = try #require(throws: UsageError.self) { try MakeAPKOptions.parse(arguments) }
        #expect(!error.message.isEmpty)
    }

    // MARK: - The layout

    @Test func takesTheManifestTableAndResFilesWithAapt2sMethods() throws {
        let archive = try ZipArchive(data: Self.pack(assets: [PackFile(name: "a.txt", contents: Data("a".utf8))]))

        #expect(archive.entries.map(\.name) == [
            "AndroidManifest.xml", "resources.arsc", "res/drawable/icon.png", "res/layout/main.xml",
            "lib/arm64-v8a/libactivity-test.so", "assets/a.txt",
        ])
        let methods = Dictionary(uniqueKeysWithValues: archive.entries.map { ($0.name, $0.method) })
        #expect(methods["resources.arsc"] == .stored)
        #expect(methods["res/drawable/icon.png"] == .stored)
        #expect(methods["res/layout/main.xml"] == .deflated)
        // Android refuses a compressed or unaligned table from targetSdk 30 on (ParsingPackageUtils,
        // INSTALL_PARSE_FAILED_RESOURCES_ARSC_COMPRESSED).
        #expect(try archive.dataOffset(of: #require(archive.entry(named: "resources.arsc"))) % 4 == 0)
        for entry in archive.entries where entry.method == .stored {
            #expect(try archive.dataOffset(of: entry) % 4 == 0, "\(entry.name)")
        }
        #expect(try archive.data(named: "res/layout/main.xml") == Data("<View/>".utf8))
        #expect(try archive.data(named: "AndroidManifest.xml") == ZipArchive(data: Self.linked()).data(named: "AndroidManifest.xml"))
    }

    @Test func putsDexFilesBeforeTheTable() throws {
        let apk = Self.linked(hasCode: true)
        let archive = try ZipArchive(data: Self.pack(apk, dex: [PackFile(name: "classes.dex", contents: LayoutTests.dex())]))

        #expect(Array(archive.entries.map(\.name).prefix(3)) == ["AndroidManifest.xml", "classes.dex", "resources.arsc"])
    }

    /// The label resolves through the table, as the import and the library read it.
    @Test func readsBackWithItsLabelInEachLanguage() throws {
        let archive = try ZipArchive(data: Self.pack())

        #expect(try APKAnalyzer.analyze(archive, preferredLanguages: ["en"]).label == "Minato check")
        #expect(try APKAnalyzer.analyze(archive, preferredLanguages: ["de"]).label == "Minato-Prüfung")
        #expect(try APKAnalyzer.analyze(archive, preferredLanguages: ["en"]).runtime == .nativeActivity(library: "activity-test"))
    }

    /// Each has an option of its own, and make-apk signs.
    @Test(arguments: ["classes.dex", "lib/arm64-v8a/libx.so", "assets/a.txt", "META-INF/CERT.RSA", "other.txt"])
    func refusesEntriesThatComeAnotherWay(name: String) throws {
        let apk = Self.linked(extra: [ZipBuilder.Entry(name: name, content: Data("x".utf8))])

        let error = try #require(throws: PackError.self) { try LinkedResources.read(apk, origin: "build/linked.apk") }

        #expect(error.file == "build/linked.apk")
        #expect(error.message.contains(name), "\(error.message)")
    }

    @Test func refusesACompressedTable() throws {
        let error = try #require(throws: PackError.self) {
            try LinkedResources.read(Self.linked(tableMethod: .deflated), origin: "build/linked.apk")
        }

        #expect(error.message.contains("resources.arsc"), "\(error.message)")
    }

    @Test(arguments: [(false, true, "resources.arsc"), (true, false, "AndroidManifest.xml")])
    func refusesAnAPKWithoutManifestOrTable(withTable: Bool, withManifest: Bool, missing: String) throws {
        let error = try #require(throws: PackError.self) {
            try LinkedResources.read(Self.linked(withTable: withTable, withManifest: withManifest), origin: "build/linked.apk")
        }

        #expect(error.message.contains(missing), "\(error.message)")
    }

    @Test func refusesWhatIsNotAZip() throws {
        let error = try #require(throws: PackError.self) {
            try LinkedResources.read(Data("not a zip".utf8), origin: "build/linked.apk")
        }

        #expect(error.file == "build/linked.apk")
    }

    /// The manifest's checks stay: aapt2's manifest without code and a DEX file disagree.
    @Test func checksAapt2sManifestLikeOurs() throws {
        let error = try #require(throws: PackError.self) {
            try Self.pack(dex: [PackFile(name: "classes.dex", contents: LayoutTests.dex())])
        }

        #expect(error.message.contains(#"android:hasCode="false""#), "\(error.message)")
    }

    // MARK: - End to end

    @Test func packsAapt2sOutputAndSignsIt() throws {
        let project = try MakeAPKTests.project()
        defer { try? FileManager.default.removeItem(at: project) }
        let linked = project.appending(path: "linked.apk")
        try Self.linked().write(to: linked)
        let apk = project.appending(path: "resources.apk")
        var errors: [String] = []

        let status = MakeAPK.run([
            "--resources", linked.path, "--lib", project.appending(path: "libactivity-test.so").path, "--out", apk.path,
        ], keyFolder: MakeAPKTests.keyFolder, output: { _ in }, errors: { errors.append($0) })

        #expect(status == 0, "\(errors)")
        let data = try Data(contentsOf: apk)
        #expect(try ZipArchive(data: data).entry(named: "resources.arsc") != nil)
        #expect(try SigningTests.apkCheck(data).contains("RESULT: signatures valid"))
    }
}
