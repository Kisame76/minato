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

    /// As ResStringPool::stringAt finds a string (ResourceTypes.cpp:808): a UTF-16 pool's offset counts whole UTF-16
    /// units, so an odd one reads from the unit it falls in. A UTF-8 pool's counts bytes.
    @Test func aUTF16OffsetCountsWholeUnits() throws {
        var chunk = StringPoolWriter.chunk(["ab", "cd"], utf8: false)
        // "cd" starts 8 bytes into the strings, behind "ab"'s length, two units and NUL; its offset is the second.
        #expect(chunk[32] == 8)
        chunk[32] = 9
        let pool = try StringPool(chunk: chunk)
        #expect(pool[0] == "ab")
        #expect(pool[1] == "cd")
    }

    // ResStringPool::setTo's checks of a whole pool (ResourceTypes.cpp:500-700 at android-15.0.0_r1): a pool that
    // fails one is BAD_TYPE, and Android reads none of its strings, so neither the manifest nor resources.arsc that
    // holds it.

    /// A pool of `strings` with one style, as aapt2 writes it: the style's END, then the two words that make the pool's
    /// closing END span. `stylesStart` overrides where the header says the styles start.
    static func styledPool(_ strings: [String], utf8: Bool, stylesStart: Int? = nil) -> Data {
        let plain = StringPoolWriter.chunk(strings, utf8: utf8)
        let count = strings.count
        let payload = plain[(28 + 4 * count)...]
        let stringsStart = 28 + 4 * count + 4
        let styles = [UInt32](repeating: 0xFFFF_FFFF, count: 3)
        let size = stringsStart + payload.count + 4 * styles.count
        var chunk = ByteWriter()
        chunk.u16(0x0001)
        chunk.u16(28)
        chunk.u32(UInt32(size))
        chunk.u32(UInt32(count))
        chunk.u32(1)
        chunk.u32(utf8 ? 0x100 : 0)
        chunk.u32(UInt32(stringsStart))
        chunk.u32(UInt32(stylesStart ?? stringsStart + payload.count))
        for index in 0..<count {
            let at = 28 + 4 * index
            chunk.u32(plain[at...].prefix(4).reversed().reduce(0) { $0 << 8 | UInt32($1) })
        }
        chunk.u32(0) // the style's place in the style pool
        chunk.bytes(Data(payload))
        styles.forEach { chunk.u32($0) }
        return chunk.data
    }

    @Test func aStyledPoolReadsItsStrings() throws {
        for utf8 in [false, true] {
            let pool = try StringPool(chunk: Self.styledPool(["ab", "cd"], utf8: utf8))
            #expect(pool[0] == "ab", "UTF-8: \(utf8)")
            #expect(pool[1] == "cd", "UTF-8: \(utf8)")
        }
    }

    /// validate_chunk (:126-143): the header and the chunk are whole 32-bit words.
    @Test func aPoolIsWholeWords() {
        var chunk = StringPoolWriter.chunk(["ab"], utf8: false)
        chunk.append(contentsOf: [0, 0])
        chunk[4] += 2
        #expect(throws: APKError.self) { try StringPool(chunk: chunk) }
    }

    /// :587-592: the strings start before the chunk's last 16-bit unit.
    @Test func theStringsStartBeforeTheLastUnit() {
        var chunk = StringPoolWriter.chunk(["ab"], utf8: false)
        chunk[20] = UInt8(chunk.count - 2)
        #expect(throws: APKError.self) { try StringPool(chunk: chunk) }
    }

    /// :598-609: the styles start after the strings and before the chunk's last 16-bit unit.
    @Test func theStylesFollowTheStrings() {
        let size = Self.styledPool(["ab"], utf8: false).count
        let stringsStart = 28 + 4 + 4
        for stylesStart in [size - 2, stringsStart, stringsStart - 4] {
            #expect(throws: APKError.self, "styles at \(stylesStart)") {
                try StringPool(chunk: Self.styledPool(["ab"], utf8: false, stylesStart: stylesStart))
            }
        }
    }

    /// :604-611 with stringAt's bound (:819): the strings end where the styles start, so a string that runs past them
    /// is unreadable, while the pool's last unit, a NUL inside that string, keeps the pool itself valid.
    @Test func theStringsEndWhereTheStylesStart() throws {
        // "x" takes 6 bytes; "a\0b" begins at 6, its length unit, "a" and the NUL inside it end at 12.
        let stringsStart = 28 + 4 * 2 + 4
        let pool = try StringPool(chunk: Self.styledPool(["x", "a\u{0}b"], utf8: false, stylesStart: stringsStart + 12))
        #expect(pool[0] == "x")
        #expect(pool[1] == nil)
    }

    /// :614-618: a pool with strings holds at least one unit. Here the strings start one byte before the styles,
    /// which end with their END span, and the byte before the strings is 0, so only this check fails.
    @Test func aPoolWithStringsHasAUnit() {
        var chunk = ByteWriter()
        for value: UInt32 in [0x001C_0001, 52, 1, 1, 0, 39, 40, 0, 0, 0] { chunk.u32(value) }
        for _ in 0..<3 { chunk.u32(0xFFFF_FFFF) }
        #expect(throws: APKError.self) { try StringPool(chunk: chunk.data) }
    }

    /// :633-645: the pool's last unit is a NUL, for UTF-16 and UTF-8 alike.
    @Test func thePoolEndsWithANUL() {
        for utf8 in [false, true] {
            var chunk = StringPoolWriter.chunk(["ab"], utf8: utf8)
            chunk[chunk.count - 1] = 0x41
            #expect(throws: APKError.self, "UTF-8: \(utf8)") { try StringPool(chunk: chunk) }
        }
    }

    /// :665-669 and :686-692: the style pool starts inside the chunk, and ends with an END span.
    @Test func theStylesEndWithAnEndSpan() {
        var chunk = Self.styledPool(["ab"], utf8: false)
        chunk[chunk.count - 1] = 0
        #expect(throws: APKError.self) { try StringPool(chunk: chunk) }
        // Without strings, only the style pool's own checks apply.
        var empty = ByteWriter()
        for value: UInt32 in [0x001C_0001, 32, 0, 1, 0, 0, 32] { empty.u32(value) }
        empty.u32(0)
        #expect(throws: APKError.self) { try StringPool(chunk: empty.data) }
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
