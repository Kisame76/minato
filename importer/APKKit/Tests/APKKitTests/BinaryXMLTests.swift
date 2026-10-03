import APKTestSupport
import Foundation
import Testing

@testable import APKKit

@Suite("BinaryXML")
struct BinaryXMLTests {
    typealias E = AXMLBuilder.Element
    typealias A = AXMLBuilder.Attribute
    static let android = AXMLBuilder.androidNamespace

    static let sampleManifest = E("manifest", [.plain("package", .string("org.example.game"))], [
        E("uses-sdk", [.android("minSdkVersion", 0x0101_020C, .int(24))]),
        E("application", [.android("label", 0x0101_0001, .reference(0x7F0A_0001))], [
            E("activity", [.android("name", 0x0101_0003, .string(".MainActivity"))]),
            E("activity", [.android("name", 0x0101_0003, .string(".SettingsActivity"))]),
        ]),
    ])

    @Test func parsesElementTree() throws {
        let root = try BinaryXML.parse(AXMLBuilder().build(Self.sampleManifest))

        #expect(root.name == "manifest")
        #expect(root.children.map(\.name) == ["uses-sdk", "application"])
        #expect(root.children(named: "application").first?.children.map(\.name) == ["activity", "activity"])
    }

    @Test func decodesTypedAttributeValues() throws {
        let element = E("feature", [
            .plain("text", .string("hello")),
            .android("minSdkVersion", 0x0101_020C, .int(-7)),
            .android("glEsVersion", 0x0101_0281, .hex(0x0003_0002)),
            .android("required", 0x0101_028E, .bool(true)),
            .android("hasCode", 0x0101_000C, .bool(false)),
            .android("icon", 0x0101_0002, .reference(0x7F08_0042)),
        ])
        let root = try BinaryXML.parse(AXMLBuilder().build(element))

        #expect(root.attribute(named: "text")?.value == .string("hello"))
        #expect(root.attribute(named: "minSdkVersion", namespace: Self.android)?.value == .integer(-7))
        #expect(root.attribute(named: "glEsVersion", namespace: Self.android)?.value == .integer(0x0003_0002))
        #expect(root.attribute(named: "required", namespace: Self.android)?.value == .boolean(true))
        #expect(root.attribute(named: "hasCode", namespace: Self.android)?.value == .boolean(false))
        #expect(root.attribute(named: "icon", namespace: Self.android)?.value == .reference(0x7F08_0042))
    }

    @Test func distinguishesNamespacedFromPlainAttributes() throws {
        let element = E("activity", [
            .plain("name", .string("plain")),
            .android("name", 0x0101_0003, .string("namespaced")),
        ])
        let root = try BinaryXML.parse(AXMLBuilder().build(element))

        #expect(root.attribute(named: "name")?.value == .string("plain"))
        #expect(root.attribute(named: "name", namespace: Self.android)?.value == .string("namespaced"))
    }

    @Test func readsUTF8StringPool() throws {
        let long = String(repeating: "x", count: 200)
        let element = E("application", [
            .android("label", 0x0101_0001, .string("Spiel äöü 🎮")),
            .plain("long", .string(long)),
        ])
        var builder = AXMLBuilder()
        builder.useUTF8 = true
        let root = try BinaryXML.parse(builder.build(element))

        #expect(root.attribute(named: "label", namespace: Self.android)?.value == .string("Spiel äöü 🎮"))
        #expect(root.attribute(named: "long")?.value == .string(long))
    }

    @Test func readsUTF16StringPoolWithNonASCII() throws {
        let element = E("application", [.android("label", 0x0101_0001, .string("Spiel äöü 🎮"))])
        let root = try BinaryXML.parse(AXMLBuilder().build(element))

        #expect(root.attribute(named: "label", namespace: Self.android)?.value == .string("Spiel äöü 🎮"))
    }

    /// As ResStringPool gives a string (ResourceTypes.cpp:799-1015): it must end with a NUL, which for a UTF-8 length
    /// past 0x7FFF that aapt cut to its low 15 bits lies a multiple of 0x8000 further.
    @Test func aStringWithoutItsNULIsUnreadable() throws {
        for utf8 in [false, true] {
            var chunk = StringPoolWriter.chunk(["first", "second"], utf8: utf8)
            // "first" starts after the header and two offsets, at 36, behind its lengths.
            chunk[36 + (utf8 ? 2 + 5 : 2 + 10)] = 0x41
            let pool = try StringPool(chunk: chunk)
            #expect(pool[0] == nil, "UTF-8: \(utf8)")
            #expect(pool[1] == "second", "UTF-8: \(utf8)")
        }
        let long = String(repeating: "y", count: 0x8005)
        let pool = try StringPool(chunk: StringPoolWriter.chunk([long, "z"], utf8: true))
        #expect(pool[0] == long)
        #expect(pool[1] == "z")
    }

    @Test func findsObfuscatedAttributeByResourceID() throws {
        // Obfuscators blank the attribute names; Android resolves attributes through the resource map.
        let element = E("manifest", [A(namespace: Self.android, name: "", resourceID: 0x0101_021B, value: .int(42))])
        let root = try BinaryXML.parse(AXMLBuilder().build(element))

        let attribute = try #require(root.attribute(resourceID: 0x0101_021B))
        #expect(attribute.value == .integer(42))
        #expect(attribute.name == "")
    }

    @Test func skipsUnknownChunks() throws {
        var builder = AXMLBuilder()
        builder.insertUnknownChunk = true
        let root = try BinaryXML.parse(builder.build(Self.sampleManifest))

        #expect(root.name == "manifest")
        #expect(root.children.count == 2)
    }

    @Test func rejectsDataThatIsNotBinaryXML() {
        #expect(throws: APKError.self) { _ = try BinaryXML.parse(Data("<manifest/>".utf8)) }
        #expect(throws: APKError.self) { _ = try BinaryXML.parse(Data()) }
    }

    @Test func survivesCorruptedInput() throws {
        let valid = AXMLBuilder().build(Self.sampleManifest)
        var generator = SplitMix64(seed: 0xA5A5)
        for _ in 0..<2_000 {
            var corrupted = valid
            for _ in 0..<Int.random(in: 1...8, using: &generator) {
                let position = Int.random(in: 0..<corrupted.count, using: &generator)
                corrupted[position] = UInt8.random(in: 0...255, using: &generator)
            }
            // Must either parse or throw; trapping fails the whole test run.
            _ = try? BinaryXML.parse(corrupted)
        }
        _ = try? BinaryXML.parse(valid.prefix(valid.count / 2))
    }
}
