import APKKit
import APKPack
import Foundation
import Testing

@Suite("ManifestCompiler")
struct ManifestCompilerTests {
    /// Every M3 test app has this shape (ADR-0031). The attributes of most elements are listed out of
    /// resource ID order on purpose: the compiler must sort them as aapt2 does.
    static let nativeActivity = """
        <?xml version="1.0" encoding="utf-8"?>
        <manifest xmlns:android="http://schemas.android.com/apk/res/android"
            package="io.github.kisame76.minato.test.activity"
            android:versionName="1.0" android:versionCode="7">
          <uses-sdk android:targetSdkVersion="35" android:minSdkVersion="29"/>
          <uses-feature android:required="true" android:glEsVersion="0x00030000"/>
          <uses-feature android:version="0x401000" android:name="android.hardware.vulkan.version"/>
          <application android:extractNativeLibs="false" android:hasCode="false" android:label="Activity Test">
            <activity android:exported="true" android:name="android.app.NativeActivity">
              <meta-data android:value="activity-test" android:name="android.app.lib_name"/>
              <intent-filter>
                <action android:name="android.intent.action.MAIN"/>
                <category android:name="android.intent.category.LAUNCHER"/>
              </intent-filter>
            </activity>
          </application>
        </manifest>
        """

    static func compile(_ text: String) throws -> Data {
        try ManifestCompiler.compile(Data(text.utf8), file: "AndroidManifest.xml")
    }

    @Test func compilesWhatAPKKitReadsBack() throws {
        let manifest = try AndroidManifest(xml: BinaryXML.parse(Self.compile(Self.nativeActivity)))

        #expect(manifest.packageName == "io.github.kisame76.minato.test.activity")
        #expect(manifest.versionCode == 7)
        #expect(manifest.versionName == .string("1.0"))
        #expect(manifest.minSdk == 29)
        #expect(manifest.targetSdk == 35)
        #expect(manifest.label == .string("Activity Test"))
        #expect(manifest.hasCode == false)
        #expect(manifest.extractNativeLibs == false)
        #expect(manifest.launcherActivity == "android.app.NativeActivity")
        #expect(manifest.nativeLibraryName == "activity-test")
        #expect(manifest.requiredGLESVersion == 0x0003_0000)
        #expect(manifest.requiredVulkanVersion == 0x0040_1000)
    }

    @Test func sortsAttributesByResourceIDAsAapt2Does() throws {
        let xml = try RawXML(Self.compile(Self.nativeActivity))

        func order(_ element: String) -> [String] {
            xml.element(element)?.attributes.map(\.name) ?? []
        }
        #expect(order("manifest") == ["versionCode", "versionName", "package"])
        #expect(order("uses-sdk") == ["minSdkVersion", "targetSdkVersion"])
        #expect(order("uses-feature") == ["glEsVersion", "required"])
        #expect(order("application") == ["label", "hasCode", "extractNativeLibs"])
        #expect(order("activity") == ["name", "exported"])
        #expect(order("meta-data") == ["name", "value"])
        #expect(xml.elements.filter { $0.name == "uses-feature" }.last?.attributes.map(\.name) == ["name", "version"])
        #expect(xml.element("manifest")?.attribute("versionCode")?.resourceID == 0x0101_021B)
        #expect(xml.element("manifest")?.attribute("package")?.resourceID == nil)
    }

    /// The constants of android.R.attr as Android's reference documentation gives them, in decimal.
    @Test func givesEveryAttributeItsIDFromAndroidRAttr() throws {
        let text = Self.manifest(line: """
            <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="35"/>
            <uses-feature android:name="f" android:required="true" android:glEsVersion="0x00030000" android:version="1"/>
            <application android:label="L" android:hasCode="false" android:extractNativeLibs="false">
            <activity android:name="a" android:exported="true"><meta-data android:name="k" android:value="v"/></activity>
            </application>
            """).replacingOccurrences(of: #"package="org.example.app""#,
                                      with: #"package="org.example.app" android:versionCode="1" android:versionName="1.0""#)
        var ids: [String: UInt32] = [:]
        for element in try RawXML(Self.compile(text)).elements {
            for attribute in element.attributes where attribute.namespace != nil {
                ids[attribute.name] = attribute.resourceID
            }
        }

        #expect(ids == [
            "label": 16_842_753,
            "name": 16_842_755,
            "hasCode": 16_842_764,
            "exported": 16_842_768,
            "value": 16_842_788,
            "minSdkVersion": 16_843_276,
            "versionCode": 16_843_291,
            "versionName": 16_843_292,
            "targetSdkVersion": 16_843_376,
            "glEsVersion": 16_843_393,
            "required": 16_843_406,
            "extractNativeLibs": 16_844_010,
            "version": 16_844_057,
        ])
    }

    @Test func encodesValuesAsAapt2Does() throws {
        let xml = try RawXML(Self.compile(Self.nativeActivity))
        let android = "http://schemas.android.com/apk/res/android"

        let versionCode = xml.element("manifest")?.attribute("versionCode")
        #expect(versionCode == RawXML.Attribute(namespace: android, name: "versionCode", resourceID: 0x0101_021B,
                                                rawValue: nil, type: 0x10, data: 7))
        let gles = xml.element("uses-feature")?.attribute("glEsVersion")
        #expect(gles?.type == 0x11)
        #expect(gles?.data == 0x0003_0000)
        let required = xml.element("uses-feature")?.attribute("required")
        #expect(required?.type == 0x12)
        #expect(required?.data == 0xFFFF_FFFF)
        let hasCode = xml.element("application")?.attribute("hasCode")
        #expect(hasCode?.type == 0x12)
        #expect(hasCode?.data == 0)
        let label = xml.element("application")?.attribute("label")
        #expect(label?.type == 0x03)
        #expect(label?.rawValue == "Activity Test")
        let package = xml.element("manifest")?.attribute("package")
        #expect(package?.namespace == nil)
        #expect(package?.type == 0x03)
        #expect(package?.rawValue == "io.github.kisame76.minato.test.activity")
    }

    @Test(arguments: [
        ("true", 0x12, 0xFFFF_FFFF, nil),
        ("TRUE", 0x12, 0xFFFF_FFFF, nil),
        ("True", 0x12, 0xFFFF_FFFF, nil),
        ("false", 0x12, 0, nil),
        ("FALSE", 0x12, 0, nil),
        ("False", 0x12, 0, nil),
        ("42", 0x10, 42, nil),
        ("-3", 0x10, 0xFFFF_FFFD, nil),
        ("0x10", 0x11, 16, nil),
        ("activity-test", 0x03, nil, "activity-test"),
    ] as [(String, UInt8, UInt32?, String?)])
    func infersTheTypeOfAMetaDataValue(value: String, type: UInt8, data: UInt32?, rawValue: String?) throws {
        let xml = try RawXML(Self.compile(Self.manifest(line: #"<meta-data android:name="key" android:value="\#(value)"/>"#)))
        let attribute = try #require(xml.element("meta-data")?.attribute("value"))

        #expect(attribute.type == type)
        if let data { #expect(attribute.data == data) }
        #expect(attribute.rawValue == rawValue)
    }

    /// aapt2's `ParseBool` accepts these spellings; Android reads the value, not the text.
    @Test(arguments: [("TRUE", true), ("True", true), ("FALSE", false), ("False", false)])
    func acceptsAapt2sSpellingsOfBooleans(text: String, flag: Bool) throws {
        let manifest = try AndroidManifest(xml: BinaryXML.parse(Self.compile(Self.manifest(
            line: #"<application android:hasCode="\#(text)"/>"#))))

        #expect(manifest.hasCode == flag)
    }

    @Test func resolvesTheAndroidNamespaceByItsURIWhateverItsPrefix() throws {
        let usual = try Self.compile(Self.nativeActivity)
        let otherPrefix = try Self.compile(Self.nativeActivity
            .replacingOccurrences(of: "xmlns:android=", with: "xmlns:a=")
            .replacingOccurrences(of: "android:", with: "a:"))

        #expect(otherPrefix == usual)
        #expect(try AndroidManifest(xml: BinaryXML.parse(otherPrefix)).versionCode == 7)
    }

    /// A manifest with `line` as its third line.
    static func manifest(line: String) -> String {
        """
        <manifest xmlns:android="http://schemas.android.com/apk/res/android"
            package="org.example.app">
        \(line)
        </manifest>
        """
    }

    @Test(arguments: [
        (#"<application android:theme="x"/>"#, "android:theme"),
        (#"<application debuggable="true"/>"#, "debuggable"),
        (#"<application xmlns:tools="http://schemas.android.com/tools" tools:ignore="x"/>"#, "tools:ignore"),
        (#"<application undeclared:label="x"/>"#, "undeclared:label"),
        (#"<dist:module xmlns:dist="http://schemas.android.com/apk/distribution"/>"#, "dist:module"),
        (#"<application android:label="@string/app_name"/>"#, "@string/app_name"),
        (#"<application android:label="?attr/label"/>"#, "?attr/label"),
        (##"<meta-data android:name="key" android:value="#ff0000"/>"##, "#ff0000"),
        (#"<meta-data android:name="key" android:value="1.5"/>"#, "1.5"),
        (#"<application android:hasCode="no"/>"#, "android:hasCode"),
        (#"<uses-sdk android:minSdkVersion="Baklava"/>"#, "android:minSdkVersion"),
        (#"<uses-sdk android:minSdkVersion="4294967296"/>"#, "android:minSdkVersion"),
        (#"<application android:hasCode=" true"/>"#, "android:hasCode"),
        (#"<meta-data android:name="key" android:value="42 "/>"#, "android:value"),
        (#"<application android:label=" Activity Test"/>"#, "android:label"),
        (#"<application android:label="Line\nBreak"/>"#, "android:label"),
        (#"<uses-sdk android:minSdkVersion="0X1D"/>"#, "android:minSdkVersion"),
        (#"<meta-data android:name="key" android:value="0X10"/>"#, "0X10"),
        (#"<application>text</application>"#, "application"),
        (#"<application><![CDATA[text]]></application>"#, "application"),
    ])
    func refusesWithFileAndLine(line: String, naming: String) throws {
        let error = try #require(throws: PackError.self) { try Self.compile(Self.manifest(line: line)) }

        #expect(error.file == "AndroidManifest.xml")
        #expect(error.line == 3)
        #expect(error.message.contains(naming))
    }

    @Test func refusesAManifestWithoutPackage() throws {
        let text = """
            <?xml version="1.0" encoding="utf-8"?>
            <manifest xmlns:android="http://schemas.android.com/apk/res/android" android:versionCode="1">
            </manifest>
            """
        let error = try #require(throws: PackError.self) { try Self.compile(text) }

        #expect(error.line == 2)
        #expect(error.message.contains("package"))
    }

    @Test(arguments: [
        (#"android:theme="x" android:label="y""#, #"android:hasCode="false""#, 4),
        (#"android:label="y" android:hasCode="false""#, #"android:theme="x""#, 5),
    ])
    func namesTheLineOfTheAttributeInATagOverSeveralLines(firstLine: String, secondLine: String, line: Int) throws {
        let text = """
            <manifest xmlns:android="http://schemas.android.com/apk/res/android"
                package="org.example.app">
            <application
                \(firstLine)
                \(secondLine)/>
            </manifest>
            """
        let error = try #require(throws: PackError.self) { try Self.compile(text) }

        #expect(error.line == line)
        #expect(error.message.contains("android:theme"))
    }

    @Test func reportsTheFirstOfSeveralProblemsInATag() throws {
        let text = Self.manifest(line: #"<application android:theme="x" android:label="@string/name" android:hint="y"/>"#)
        let error = try #require(throws: PackError.self) { try Self.compile(text) }

        #expect(error.message.contains("android:theme"))
    }

    @Test func namesTheFirstLineOfATagOverSeveralLines() throws {
        let text = """
            <!-- 2 > 1, and <manifest stands in this comment -->
            <manifest
                xmlns:android="http://schemas.android.com/apk/res/android"
                android:versionCode="1">
            </manifest>
            """
        let error = try #require(throws: PackError.self) { try Self.compile(text) }

        #expect(error.line == 2)
        #expect(error.message.contains("package"))
    }

    @Test func refusesTheSameAttributeTwiceThroughTwoPrefixes() throws {
        let text = """
            <manifest xmlns:android="http://schemas.android.com/apk/res/android"
                package="org.example.app">
            <application xmlns:a="http://schemas.android.com/apk/res/android" android:label="x" a:label="y"/>
            </manifest>
            """
        let error = try #require(throws: PackError.self) { try Self.compile(text) }

        #expect(error.line == 3)
        #expect(error.message.contains("android:label"))
    }

    /// aapt2's rule for the package name (`IsAndroidPackageName`): at least two segments, each a
    /// letter followed by letters, digits or underscores. Minato's library refuses empty segments too.
    @Test(arguments: ["", "app", "org.1example", "org..example", "org.example-app", "org._example", ".org.example"])
    func refusesPackageNamesAndroidRefuses(name: String) throws {
        let text = #"<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="\#(name)"/>"#
        let error = try #require(throws: PackError.self) { try Self.compile(text) }

        #expect(error.line == 1)
        #expect(error.message.contains("package"))
    }

    @Test(arguments: ["org.example", "io.github.kisame76.minato.test_app", "A.b2"])
    func acceptsPackageNamesAndroidAccepts(name: String) throws {
        let text = #"<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="\#(name)"/>"#

        #expect(try AndroidManifest(xml: BinaryXML.parse(Self.compile(text))).packageName == name)
    }

    @Test func refusesARootOtherThanManifest() throws {
        // With a package, so that only the check of the root can refuse it.
        let error = try #require(throws: PackError.self) {
            try Self.compile(#"<application xmlns:android="http://schemas.android.com/apk/res/android" package="org.example.app"/>"#)
        }

        #expect(error.line == 1)
        #expect(error.message.contains("<application>"))
    }

    @Test func refusesXMLThatDoesNotParse() throws {
        let error = try #require(throws: PackError.self) {
            try Self.compile(Self.manifest(line: "<application>") )
        }

        #expect(error.file == "AndroidManifest.xml")
        #expect(error.line == 4)
    }
}
