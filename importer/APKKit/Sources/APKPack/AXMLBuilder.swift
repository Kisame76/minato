import Foundation

/// Encodes Android binary XML (ResXMLTree) the way aapt2 lays it out.
public struct AXMLBuilder {
    public static let androidNamespace = "http://schemas.android.com/apk/res/android"

    public enum Value: Sendable {
        case string(String)
        case int(Int32)
        case hex(UInt32)
        case bool(Bool)
        case reference(UInt32)
    }

    public struct Attribute: Sendable {
        public var namespace: String?
        public var name: String
        public var resourceID: UInt32?
        public var value: Value

        public init(namespace: String?, name: String, resourceID: UInt32?, value: Value) {
            self.namespace = namespace
            self.name = name
            self.resourceID = resourceID
            self.value = value
        }

        /// `android:` attribute with its framework resource ID.
        public static func android(_ name: String, _ id: UInt32?, _ value: Value) -> Attribute {
            Attribute(namespace: AXMLBuilder.androidNamespace, name: name, resourceID: id, value: value)
        }

        /// Attribute without namespace, e.g. `package` on `<manifest>`.
        public static func plain(_ name: String, _ value: Value) -> Attribute {
            Attribute(namespace: nil, name: name, resourceID: nil, value: value)
        }
    }

    public struct Element: Sendable {
        public var name: String
        public var attributes: [Attribute]
        public var children: [Element]

        public init(_ name: String, _ attributes: [Attribute] = [], _ children: [Element] = []) {
            self.name = name
            self.attributes = attributes
            self.children = children
        }
    }

    public var useUTF8 = false
    /// Adds a chunk of an unknown type before the root element.
    public var insertUnknownChunk = false

    public init() {}

    public func build(_ root: Element) -> Data {
        var pool = Pool()
        // aapt2 puts attribute names that carry a resource ID first; the resource map covers exactly them.
        func collectIDs(_ element: Element) {
            for attribute in element.attributes where attribute.resourceID != nil {
                pool.addIdentified(attribute.name, attribute.resourceID!)
            }
            element.children.forEach(collectIDs)
        }
        func collectStrings(_ element: Element) {
            pool.add(element.name)
            for attribute in element.attributes {
                if attribute.resourceID == nil { pool.add(attribute.name) }
                if let namespace = attribute.namespace { pool.add(namespace) }
                if case .string(let text) = attribute.value { pool.add(text) }
            }
            element.children.forEach(collectStrings)
        }
        collectIDs(root)
        pool.add("android")
        pool.add(Self.androidNamespace)
        collectStrings(root)

        var body = ByteWriter()
        body.bytes(StringPoolWriter.chunk(pool.strings, utf8: useUTF8))

        body.u16(0x0180)
        body.u16(8)
        body.u32(UInt32(8 + 4 * pool.identifiers.count))
        pool.identifiers.forEach { body.u32($0) }

        if insertUnknownChunk {
            body.u16(0x0777)
            body.u16(8)
            body.u32(16)
            body.u64(0xDEAD_BEEF)
        }

        writeNamespace(&body, type: 0x0100, pool: pool)
        writeElement(root, into: &body, pool: pool)
        writeNamespace(&body, type: 0x0101, pool: pool)

        var document = ByteWriter()
        document.u16(0x0003)
        document.u16(8)
        document.u32(UInt32(8 + body.count))
        document.bytes(body.data)
        return document.data
    }

    private func writeNamespace(_ out: inout ByteWriter, type: UInt16, pool: Pool) {
        out.u16(type)
        out.u16(16)
        out.u32(24)
        out.u32(1)
        out.u32(0xFFFF_FFFF)
        out.u32(pool.index("android"))
        out.u32(pool.index(Self.androidNamespace))
    }

    private func writeElement(_ element: Element, into out: inout ByteWriter, pool: Pool) {
        out.u16(0x0102)
        out.u16(16)
        out.u32(UInt32(16 + 20 + 20 * element.attributes.count))
        out.u32(1)
        out.u32(0xFFFF_FFFF)
        out.u32(0xFFFF_FFFF) // element namespace
        out.u32(pool.index(element.name))
        out.u16(20)
        out.u16(20)
        out.u16(UInt16(element.attributes.count))
        out.u16(0)
        out.u16(0)
        out.u16(0)
        for attribute in element.attributes {
            out.u32(attribute.namespace.map { pool.index($0) } ?? 0xFFFF_FFFF)
            out.u32(attribute.resourceID.map { pool.identifiedIndex(attribute.name, $0) } ?? pool.index(attribute.name))
            if case .string(let text) = attribute.value {
                out.u32(pool.index(text))
            } else {
                out.u32(0xFFFF_FFFF)
            }
            out.u16(8)
            out.u8(0)
            switch attribute.value {
            case .string(let text):
                out.u8(0x03)
                out.u32(pool.index(text))
            case .int(let number):
                out.u8(0x10)
                out.i32(number)
            case .hex(let number):
                out.u8(0x11)
                out.u32(number)
            case .bool(let flag):
                out.u8(0x12)
                out.u32(flag ? 0xFFFF_FFFF : 0)
            case .reference(let id):
                out.u8(0x01)
                out.u32(id)
            }
        }
        for child in element.children {
            writeElement(child, into: &out, pool: pool)
        }
        out.u16(0x0103)
        out.u16(16)
        out.u32(24)
        out.u32(1)
        out.u32(0xFFFF_FFFF)
        out.u32(0xFFFF_FFFF)
        out.u32(pool.index(element.name))
    }

    private struct Pool {
        var strings: [String] = []
        var identifiers: [UInt32] = []
        private var plainIndex: [String: Int] = [:]
        private var identifiedIndexes: [String: Int] = [:]

        mutating func addIdentified(_ name: String, _ id: UInt32) {
            let key = "\(name)#\(id)"
            guard identifiedIndexes[key] == nil else { return }
            identifiedIndexes[key] = strings.count
            strings.append(name)
            identifiers.append(id)
        }

        mutating func add(_ string: String) {
            guard plainIndex[string] == nil else { return }
            plainIndex[string] = strings.count
            strings.append(string)
        }

        func index(_ string: String) -> UInt32 { UInt32(plainIndex[string]!) }

        func identifiedIndex(_ name: String, _ id: UInt32) -> UInt32 {
            UInt32(identifiedIndexes["\(name)#\(id)"]!)
        }
    }
}
