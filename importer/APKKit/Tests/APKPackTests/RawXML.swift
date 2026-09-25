import Foundation

/// Reads the start elements of Android binary XML chunk by chunk, keeping what APKKit's parser folds
/// away: the order of each element's attributes and the exact value type (decimal or hex). Written
/// from ResourceTypes.h for these tests, independently of APKKit's `BinaryXML`.
struct RawXML {
    struct Attribute: Equatable {
        var namespace: String?
        var name: String
        var resourceID: UInt32?
        var rawValue: String?
        var type: UInt8
        var data: UInt32
    }

    struct Element {
        var name: String
        var attributes: [Attribute]

        func attribute(_ name: String) -> Attribute? {
            attributes.first { $0.name == name }
        }
    }

    enum Failure: Error {
        case malformed(String)
    }

    var elements: [Element] = []

    init(_ data: Data) throws {
        let bytes = [UInt8](data)
        func u16(_ at: Int) throws -> Int {
            guard at >= 0, at + 2 <= bytes.count else { throw Failure.malformed("u16 at \(at)") }
            return Int(bytes[at]) | Int(bytes[at + 1]) << 8
        }
        func u32(_ at: Int) throws -> UInt32 {
            guard at >= 0, at + 4 <= bytes.count else { throw Failure.malformed("u32 at \(at)") }
            return (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[at + $1]) << (8 * UInt32($1)) }
        }

        guard try u16(0) == 0x0003 else { throw Failure.malformed("not a ResXMLTree") }
        var strings: [String] = []
        var resourceIDs: [UInt32] = []
        var offset = try u16(2)
        while offset < bytes.count {
            let type = try u16(offset)
            let headerSize = try u16(offset + 2)
            let size = Int(try u32(offset + 4))
            guard size >= 8, offset + size <= bytes.count else { throw Failure.malformed("chunk size \(size) at \(offset)") }
            switch type {
            case 0x0001:
                let count = Int(try u32(offset + 8))
                let utf8 = try u32(offset + 16) & 0x100 != 0
                let stringsStart = offset + Int(try u32(offset + 20))
                for index in 0..<count {
                    var at = stringsStart + Int(try u32(offset + headerSize + 4 * index))
                    if utf8 {
                        at += bytes[at] & 0x80 != 0 ? 2 : 1
                        var length = Int(bytes[at])
                        if length & 0x80 != 0 {
                            length = (length & 0x7F) << 8 | Int(bytes[at + 1])
                            at += 1
                        }
                        at += 1
                        strings.append(String(decoding: bytes[at..<at + length], as: UTF8.self))
                    } else {
                        var length = try u16(at)
                        at += 2
                        if length & 0x8000 != 0 {
                            length = (length & 0x7FFF) << 16 | (try u16(at))
                            at += 2
                        }
                        let units = try (0..<length).map { UInt16(try u16(at + 2 * $0)) }
                        strings.append(String(decoding: units, as: UTF16.self))
                    }
                }
            case 0x0180:
                resourceIDs = try (0..<(size - headerSize) / 4).map { try u32(offset + headerSize + 4 * $0) }
            case 0x0102:
                func string(_ index: UInt32) -> String? {
                    index == 0xFFFF_FFFF ? nil : strings[Int(index)]
                }
                let ext = offset + headerSize
                guard let name = string(try u32(ext + 4)) else { throw Failure.malformed("element without a name") }
                let attributeStart = try u16(ext + 8)
                let attributeSize = try u16(ext + 10)
                let attributeCount = try u16(ext + 12)
                var attributes: [Attribute] = []
                for index in 0..<attributeCount {
                    let at = ext + attributeStart + attributeSize * index
                    let nameIndex = try u32(at + 4)
                    attributes.append(Attribute(
                        namespace: string(try u32(at)),
                        name: strings[Int(nameIndex)],
                        resourceID: Int(nameIndex) < resourceIDs.count ? resourceIDs[Int(nameIndex)] : nil,
                        rawValue: string(try u32(at + 8)),
                        type: bytes[at + 15],
                        data: try u32(at + 16)))
                }
                elements.append(Element(name: name, attributes: attributes))
            default:
                break
            }
            offset += size
        }
    }

    func element(_ name: String) -> Element? {
        elements.first { $0.name == name }
    }
}
