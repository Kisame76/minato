import Foundation

/// Encodes an Android ResStringPool chunk (frameworks/base libandroidfw ResourceTypes.h).
public enum StringPoolWriter {
    public static func chunk(_ strings: [String], utf8: Bool) -> Data {
        var payload = ByteWriter()
        var offsets: [UInt32] = []
        for string in strings {
            offsets.append(UInt32(payload.count))
            if utf8 {
                let bytes = Array(string.utf8)
                writeUTF8Length(&payload, string.utf16.count)
                writeUTF8Length(&payload, bytes.count)
                payload.bytes(bytes)
                payload.u8(0)
            } else {
                let units = Array(string.utf16)
                if units.count > 0x7FFF {
                    payload.u16(0x8000 | UInt16(units.count >> 16))
                    payload.u16(UInt16(units.count & 0xFFFF))
                } else {
                    payload.u16(UInt16(units.count))
                }
                units.forEach { payload.u16($0) }
                payload.u16(0)
            }
        }
        payload.pad(toMultipleOf: 4)

        let headerSize = 28
        let stringsStart = headerSize + 4 * strings.count
        var chunk = ByteWriter()
        chunk.u16(0x0001)
        chunk.u16(UInt16(headerSize))
        chunk.u32(UInt32(stringsStart + payload.count))
        chunk.u32(UInt32(strings.count))
        chunk.u32(0) // styles
        chunk.u32(utf8 ? 0x100 : 0)
        chunk.u32(UInt32(stringsStart))
        chunk.u32(0)
        offsets.forEach { chunk.u32($0) }
        chunk.bytes(payload.data)
        return chunk.data
    }

    private static func writeUTF8Length(_ writer: inout ByteWriter, _ length: Int) {
        if length > 0x7F {
            writer.u8(UInt8(0x80 | (length >> 8)))
            writer.u8(UInt8(length & 0xFF))
        } else {
            writer.u8(UInt8(length))
        }
    }
}
