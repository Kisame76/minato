import Foundation

public struct XMLAttribute: Sendable, Equatable {
    public let namespace: String?
    public let name: String
    /// Framework attribute ID from the resource map, e.g. 0x0101021b for `android:versionCode`.
    public let resourceID: UInt32?
    public let value: ResourceValue
    /// Original string form, if the compiler kept one.
    public let rawString: String?
    /// The raw string's index as the file holds it, `rawString` or not: Android reads one at any index below
    /// 0x8000_0000 (XmlBlock.java:287).
    public let rawIndex: UInt32
    /// The Res_value as the file holds it, which `value` decodes: its type and its data.
    public let dataType: UInt8
    public let data: UInt32
}

public struct XMLElement: Sendable, Equatable {
    public let namespace: String?
    public let name: String
    public internal(set) var attributes: [XMLAttribute]
    public internal(set) var children: [XMLElement]

    public func children(named name: String) -> [XMLElement] {
        children.filter { $0.name == name }
    }

    /// Attribute by name; `namespace == nil` matches only attributes without namespace.
    public func attribute(named name: String, namespace: String? = nil) -> XMLAttribute? {
        attributes.first { $0.name == name && $0.namespace == namespace }
    }

    public func attribute(resourceID: UInt32) -> XMLAttribute? {
        attributes.first { $0.resourceID == resourceID }
    }
}

/// Parser for Android binary XML (ResXMLTree), as found in AndroidManifest.xml and compiled res/*.xml.
public enum BinaryXML {
    public static func parse(_ data: Data) throws -> XMLElement {
        let reader = ByteReader(data, context: "binary XML")
        guard reader.count >= 8, try reader.u16(at: 0) == 0x0003 else {
            throw APKError.invalidBinaryXML("missing XML header")
        }
        let headerSize = Int(try reader.u16(at: 2))
        let declaredSize = Int(try reader.u32(at: 4))
        guard headerSize >= 8, declaredSize >= headerSize else {
            throw APKError.invalidBinaryXML("malformed XML header")
        }
        let end = min(declaredSize, reader.count)

        var strings = StringPool.empty
        var resourceIDs: [UInt32] = []
        var open: [XMLElement] = []
        var root: XMLElement?
        var position = headerSize

        while position + 8 <= end {
            let type = try reader.u16(at: position)
            let chunkHeaderSize = Int(try reader.u16(at: position + 2))
            let chunkSize = Int(try reader.u32(at: position + 4))
            guard chunkHeaderSize >= 8, chunkSize >= chunkHeaderSize, chunkSize <= end - position else {
                throw APKError.invalidBinaryXML("malformed chunk at offset \(position)")
            }
            let chunk = reader.data.subdata(in: position..<position + chunkSize)

            switch type {
            case 0x0001:
                strings = try StringPool(chunk: chunk)
            case 0x0180:
                var map = ByteReader(chunk, offset: chunkHeaderSize, context: "resource map")
                resourceIDs = try (0..<(chunkSize - chunkHeaderSize) / 4).map { _ in try map.u32() }
            case 0x0102:
                open.append(try startElement(chunk, headerSize: chunkHeaderSize, strings: strings, resourceIDs: resourceIDs))
            case 0x0103:
                if let element = open.popLast() {
                    close(element, into: &open, root: &root)
                }
            default:
                break // namespaces, CDATA and unknown chunks carry nothing we need
            }
            position += chunkSize
        }
        // Tolerate documents that end without closing their elements.
        while let element = open.popLast() {
            close(element, into: &open, root: &root)
        }
        guard let root else { throw APKError.invalidBinaryXML("no root element") }
        return root
    }

    private static func close(_ element: XMLElement, into open: inout [XMLElement], root: inout XMLElement?) {
        if open.isEmpty {
            if root == nil { root = element }
        } else {
            open[open.count - 1].children.append(element)
        }
    }

    private static func startElement(_ chunk: Data, headerSize: Int, strings: StringPool, resourceIDs: [UInt32]) throws -> XMLElement {
        var reader = ByteReader(chunk, offset: headerSize, context: "start element")
        let extensionStart = headerSize
        let namespace = strings[try reader.u32()]
        let name = strings[try reader.u32()] ?? ""
        let attributeStart = Int(try reader.u16())
        let attributeSize = Int(try reader.u16())
        let attributeCount = Int(try reader.u16())
        guard attributeSize >= 20 || attributeCount == 0 else {
            throw APKError.invalidBinaryXML("attribute size \(attributeSize)")
        }

        var attributes: [XMLAttribute] = []
        attributes.reserveCapacity(attributeCount)
        for index in 0..<attributeCount {
            try reader.seek(to: extensionStart + attributeStart + index * attributeSize)
            let namespaceIndex = try reader.u32()
            let nameIndex = try reader.u32()
            let rawIndex = try reader.u32()
            try reader.skip(3) // Res_value size and res0
            let type = try reader.u8()
            let data = try reader.u32()
            attributes.append(XMLAttribute(
                namespace: strings[namespaceIndex],
                name: strings[nameIndex] ?? "",
                resourceID: Int(nameIndex) < resourceIDs.count ? resourceIDs[Int(nameIndex)] : nil,
                value: ResourceValue(type: type, data: data, strings: strings),
                rawString: strings[rawIndex],
                rawIndex: rawIndex,
                dataType: type,
                data: data))
        }
        return XMLElement(namespace: namespace, name: name, attributes: attributes, children: [])
    }
}
