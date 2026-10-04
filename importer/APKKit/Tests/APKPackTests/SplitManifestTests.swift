import APKKit
import APKPack
import Foundation
import Testing

/// make-apk's split APKs (ADR-0048 b8, adds to ADR-0031): the manifest attributes of a split set, and a
/// NativeActivity base whose library lies in a split.
@Suite("Split manifests")
struct SplitManifestTests {
    static func manifest(_ attributes: String, application: String = #"<application android:hasCode="false"/>"#) -> String {
        """
        <manifest xmlns:android="http://schemas.android.com/apk/res/android"
            package="org.example.app" android:versionCode="1" \(attributes)>
        \(application)
        </manifest>
        """
    }

    static func compile(_ text: String) throws -> Data {
        try ManifestCompiler.compile(Data(text.utf8), file: "AndroidManifest.xml")
    }

    static func read(_ text: String) throws -> AndroidManifest {
        try AndroidManifest(xml: BinaryXML.parse(compile(text)))
    }

    @Test func compilesTheSetsManifestsThatAPKKitReadsBack() throws {
        let base = try Self.read(Self.manifest(#"android:isolatedSplits="true" android:isSplitRequired="true" "#
            + #"android:requiredSplitTypes="base__abi,base__density""#))
        let feature = try Self.read(Self.manifest(#"split="feature_extra" android:isFeatureSplit="true""#))
        let config = try Self.read(Self.manifest(#"split="feature_extra.config.de" configForSplit="feature_extra" "#
            + #"android:splitTypes="feature_extra__language""#))

        #expect(base.splitName == nil)
        #expect(base.splitRequired)
        #expect(base.requiredSplitTypes == ["base__abi", "base__density"])
        #expect(base.requiresSplits)
        #expect(feature.splitName == "feature_extra")
        #expect(feature.isFeatureSplit)
        #expect(feature.configForSplit == nil)
        #expect(config.splitName == "feature_extra.config.de")
        #expect(config.isFeatureSplit == false)
        #expect(config.configForSplit == "feature_extra")
        #expect(config.splitTypes == ["feature_extra__language"])
    }

    /// The IDs in AOSP's public-final.xml (android-15.0.0_r1): `split` and `configForSplit` have none, as Android reads
    /// them without a namespace (`ApkLiteParseUtils.java:437, 711`).
    @Test func writesEachAttributeAsAndroidReadsIt() throws {
        let android = "http://schemas.android.com/apk/res/android"
        let xml = try RawXML(Self.compile(Self.manifest(
            #"split="config.arm64_v8a" configForSplit="feature_extra" android:isolatedSplits="true" "#
                + #"android:isFeatureSplit="false" android:isSplitRequired="true" android:requiredSplitTypes="base__abi" "#
                + #"android:splitTypes="base__abi""#)))
        let manifest = try #require(xml.element("manifest"))

        #expect(manifest.attribute("isolatedSplits") == RawXML.Attribute(
            namespace: android, name: "isolatedSplits", resourceID: 0x0101_054B, rawValue: nil, type: 0x12, data: 0xFFFF_FFFF))
        #expect(manifest.attribute("isFeatureSplit") == RawXML.Attribute(
            namespace: android, name: "isFeatureSplit", resourceID: 0x0101_055B, rawValue: nil, type: 0x12, data: 0))
        #expect(manifest.attribute("isSplitRequired") == RawXML.Attribute(
            namespace: android, name: "isSplitRequired", resourceID: 0x0101_0591, rawValue: nil, type: 0x12, data: 0xFFFF_FFFF))
        #expect(manifest.attribute("requiredSplitTypes")?.resourceID == 0x0101_064E)
        #expect(manifest.attribute("requiredSplitTypes")?.type == 0x03)
        #expect(manifest.attribute("requiredSplitTypes")?.rawValue == "base__abi")
        #expect(manifest.attribute("splitTypes")?.resourceID == 0x0101_064F)
        #expect(manifest.attribute("splitTypes")?.rawValue == "base__abi")
        for name in ["split", "configForSplit"] {
            let attribute = try #require(manifest.attribute(name))
            #expect(attribute.namespace == nil)
            #expect(attribute.resourceID == nil)
            #expect(attribute.type == 0x03)
        }
        #expect(manifest.attribute("split")?.rawValue == "config.arm64_v8a")
        #expect(manifest.attribute("configForSplit")?.rawValue == "feature_extra")
        // As aapt2 sorts them: by resource ID, then the ones without by namespace and name.
        #expect(manifest.attributes.map(\.name) == [
            "versionCode", "isolatedSplits", "isFeatureSplit", "isSplitRequired", "requiredSplitTypes", "splitTypes",
            "configForSplit", "package", "split",
        ])
    }

    @Test(arguments: [#"<application split="x"/>"#, #"<application configForSplit="x"/>"#, #"<uses-split split="x"/>"#])
    func refusesTheSplitsAttributesOutsideTheManifest(line: String) throws {
        let error = try #require(throws: PackError.self) { try Self.compile(ManifestCompilerTests.manifest(line: line)) }

        #expect(error.line == 3)
        #expect(error.message.contains("unknown attribute"))
    }

    /// `FrameworkParsingPackageUtils.validateName` without a separator (`ApkLiteParseUtils.java:716-720`): each dot-separated
    /// part starts with an ASCII letter, followed by ASCII letters, digits or '_'.
    @Test(arguments: [("1abc", "'1'"), ("config.1x", "'1'"), ("config-x", "'-'"), ("_x", "'_'"), ("con fig", "' '"),
                      ("café", "'é'")])
    func refusesSplitNamesAndroidRefuses(name: String, character: String) throws {
        let text = """
            <manifest xmlns:android="http://schemas.android.com/apk/res/android"
                package="org.example.app"
                split="\(name)">
            </manifest>
            """
        let error = try #require(throws: PackError.self) { try Self.compile(text) }

        #expect(error.line == 3)
        #expect(error.message.contains("split \"\(name)\""))
        #expect(error.message.contains("bad character \(character)"))
    }

    @Test(arguments: ["config.arm64_v8a", "feature_extra", "config.xxhdpi", "Base.Feature_2"])
    func acceptsSplitNamesAndroidAccepts(name: String) throws {
        #expect(try Self.read(Self.manifest(#"split="\#(name)""#)).splitName == name)
    }

    /// Android takes `configForSplit` as it stands, so make-apk writes it so and refuses what aapt2 might change.
    @Test(arguments: [(" feature", "spaces"), ("feature ", "spaces"), (#"fea\ture"#, "backslash")])
    func refusesAConfigForSplitItWouldWriteDifferently(value: String, problem: String) throws {
        let text = """
            <manifest xmlns:android="http://schemas.android.com/apk/res/android"
                package="org.example.app" split="config.de"
                configForSplit="\(value)">
            </manifest>
            """
        let error = try #require(throws: PackError.self) { try Self.compile(text) }

        #expect(error.line == 3)
        #expect(error.message.contains("configForSplit: \(problem)"))
    }

    /// `separateAndValidateSplitTypes` (`ApkLiteParseUtils.java:757-773`): Android refuses the APK, so make-apk names the
    /// line with Android's text.
    @Test(arguments: [("requiredSplitTypes", "base__abi,1x"), ("splitTypes", "base__abi,a/b")])
    func refusesSplitTypesAndroidRefuses(attribute: String, value: String) throws {
        let text = """
            <manifest xmlns:android="http://schemas.android.com/apk/res/android"
                package="org.example.app"
                android:\(attribute)="\(value)">
            </manifest>
            """
        let error = try #require(throws: PackError.self) { try Self.compile(text) }

        #expect(error.line == 3)
        #expect(error.message.contains("Invalid manifest split types"))
    }

    /// A NativeActivity base with its library in `config.arm64_v8a`, for test 8 of step c.
    @Test(arguments: [#"android:isSplitRequired="true""#, #"android:requiredSplitTypes="base__abi""#])
    func buildsANativeActivityBaseThatLeavesItsLibraryToASplit(attribute: String) throws {
        let text = ManifestCompilerTests.nativeActivity.replacingOccurrences(
            of: #"android:versionCode="7""#, with: #"android:versionCode="7" \#(attribute)"#)

        let archive = try ZipArchive(data: APKLayout.build(manifest: Self.compile(text), libraries: [], assets: []))

        #expect(archive.entries.map(\.name) == ["AndroidManifest.xml"])
    }

    @Test func storesASplitsLibraryAlignedAsTheBaseDoes() throws {
        let text = Self.manifest(#"split="config.arm64_v8a" android:splitTypes="base__abi""#,
                                 application: #"<application android:hasCode="false" android:extractNativeLibs="false"/>"#)

        let data = try APKLayout.build(manifest: Self.compile(text),
                                       libraries: [PackFile(name: "libactivity-test.so", contents: LayoutTests.library())],
                                       assets: [])

        let archive = try ZipArchive(data: data)
        let library = try #require(archive.entry(named: "lib/arm64-v8a/libactivity-test.so"))
        #expect(library.method == .stored)
        #expect(try archive.dataOffset(of: library) % 16_384 == 0)
        #expect(try AndroidManifest(xml: BinaryXML.parse(archive.data(named: "AndroidManifest.xml"))).splitName
            == "config.arm64_v8a")
    }

    /// Android checks a split's code as the base's (`ScanPackageUtils.java:589-598`).
    @Test func refusesASplitThatDeclaresCodeWithoutDex() throws {
        let text = Self.manifest(#"split="config.arm64_v8a""#, application: "<application/>")

        let error = try #require(throws: PackError.self) {
            try APKLayout.build(manifest: Self.compile(text), libraries: [], assets: [])
        }

        #expect(error.file == "classes.dex")
    }

    @Test func packsAndSignsASplit() throws {
        let project = try MakeAPKTests.project()
        defer { try? FileManager.default.removeItem(at: project) }
        let manifest = project.appending(path: "split.xml")
        try Data(Self.manifest(#"split="config.arm64_v8a" android:splitTypes="base__abi""#,
                               application: #"<application android:hasCode="false" android:extractNativeLibs="false"/>"#)
            .utf8).write(to: manifest)
        let apk = project.appending(path: "split_config.arm64_v8a.apk")
        var errors: [String] = []

        let status = MakeAPK.run(["--manifest", manifest.path, "--lib", project.appending(path: "libactivity-test.so").path,
                                  "--out", apk.path],
                                 keyFolder: MakeAPKTests.keyFolder, output: { _ in }, errors: { errors.append($0) })

        #expect(status == 0)
        #expect(errors.isEmpty)
        let archive = try ZipArchive(data: Data(contentsOf: apk))
        #expect(archive.entries.map(\.name) == ["AndroidManifest.xml", "lib/arm64-v8a/libactivity-test.so"])
        #expect(SignatureVerifier.verify(archive, targetSdk: 35).check == .verified)
    }
}
