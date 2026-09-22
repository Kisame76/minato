import Foundation

/// Decoded Res_value (libandroidfw ResourceTypes.h).
public enum ResourceValue: Sendable, Equatable {
    case null
    /// `@type/name` reference to a resource ID.
    case reference(UInt32)
    /// `?attr` reference to a theme attribute.
    case attributeReference(UInt32)
    case string(String)
    case float(Float)
    case dimension(UInt32)
    case fraction(UInt32)
    case integer(Int32)
    case boolean(Bool)
    case color(UInt32)
    case other(type: UInt8, data: UInt32)

    init(type: UInt8, data: UInt32, strings: StringPool) {
        switch type {
        case 0x00: self = .null
        case 0x01, 0x07: self = .reference(data)
        case 0x02, 0x08: self = .attributeReference(data)
        case 0x03: self = strings[data].map(ResourceValue.string) ?? .null
        case 0x04: self = .float(Float(bitPattern: data))
        case 0x05: self = .dimension(data)
        case 0x06: self = .fraction(data)
        case 0x10, 0x11: self = .integer(Int32(bitPattern: data))
        case 0x12: self = .boolean(data != 0)
        case 0x1C...0x1F: self = .color(data)
        default: self = .other(type: type, data: data)
        }
    }
}
