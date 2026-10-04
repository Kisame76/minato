import Foundation

/// ResStringPool chunk, shared by binary XML and resources.arsc.
///
/// A string that cannot be decoded becomes `nil` instead of failing the whole pool: obfuscators
/// plant broken entries that Android itself only rejects if they are actually used. As ResStringPool
/// gives a string (ResourceTypes.cpp:799-1015), one without its NUL cannot be decoded either.
struct StringPool: Sendable {
    private let strings: [String?]

    static let empty = StringPool(strings: [])

    private init(strings: [String?]) {
        self.strings = strings
    }

    var count: Int { strings.count }

    subscript(index: UInt32) -> String? {
        guard index != 0xFFFF_FFFF, Int(index) < strings.count else { return nil }
        return strings[Int(index)]
    }

    /// `chunk` starts at the ResChunk_header of the pool. A pool that fails one of ResStringPool::setTo's checks
    /// (ResourceTypes.cpp:500-700) throws, as Android reads none of its strings then.
    init(chunk: Data) throws {
        var reader = ByteReader(chunk, context: "string pool")
        guard try reader.u16() == 0x0001 else { throw APKError.invalidBinaryXML("expected string pool") }
        let headerSize = Int(try reader.u16())
        let size = Int(try reader.u32())
        let stringCount = Int(try reader.u32())
        let styleCount = Int(try reader.u32())
        let flags = try reader.u32()
        let stringsStart = Int(try reader.u32())
        let stylesStart = Int(try reader.u32())
        // validate_chunk (:126-143) and :560-578: whole 32-bit words inside the data, with room for the entries.
        guard headerSize >= 28, size <= reader.count, headerSize <= size, (headerSize | size) & 3 == 0,
              stringCount <= (size - headerSize) / 4
        else { throw APKError.invalidBinaryXML("malformed string pool header") }

        let isUTF8 = flags & 0x100 != 0
        let unit = isUTF8 ? 1 : 2
        var end = stringsStart
        if stringCount > 0 {
            // :587-618: the strings start before the chunk's last 16-bit unit and run up to the styles where there
            // are any, at least one unit. :598-609's checks that the styles start after the strings and before that
            // unit follow from the unit and from the END span below.
            let units = ((styleCount == 0 ? size : stylesStart) - stringsStart) / unit
            guard stringsStart < size - 2, units > 0 else { throw APKError.invalidBinaryXML("an empty string pool") }
            end = stringsStart + units * unit
            // :633-645: the pool's last unit is a NUL.
            guard try (isUTF8 ? Int(reader.u8(at: end - 1)) : Int(reader.u16(at: end - 2))) == 0
            else { throw APKError.invalidBinaryXML("a string pool without its closing NUL") }
        }
        if styleCount > 0 {
            // :665-692: the style pool starts inside the chunk and ends with an END span, three words of 0xFFFFFFFF.
            let words = stylesStart < size ? (size - stylesStart) / 4 : 0
            let endSpan = { (word: Int) in try reader.u32(at: stylesStart + 4 * (words - word)) == 0xFFFF_FFFF }
            guard words >= 3, try (1...3).allSatisfy(endSpan)
            else { throw APKError.invalidBinaryXML("a style pool without its END span") }
        }

        var decoded: [String?] = []
        decoded.reserveCapacity(stringCount)
        try reader.seek(to: headerSize)
        for _ in 0..<stringCount {
            // A UTF-16 pool's offset counts whole units, as ResStringPool divides it by 2 (:808).
            let offset = Int(try reader.u32())
            let position = stringsStart + (isUTF8 ? offset : offset & ~1)
            decoded.append(try? Self.decode(reader.data, at: position, end: end, utf8: isUTF8))
        }
        strings = decoded
    }

    private static func decode(_ data: Data, at position: Int, end: Int, utf8: Bool) throws -> String {
        var reader = ByteReader(data.prefix(end), context: "string")
        try reader.seek(to: position)
        if utf8 {
            _ = try utf8Length(&reader) // length in UTF-16 units, not needed
            var byteCount = try utf8Length(&reader)
            // aapt cut a length past 0x7FFF to its low 15 bits, so the NUL may lie a multiple of 0x8000 further.
            while try reader.u8(at: reader.offset + byteCount) != 0 {
                byteCount += 0x8000
            }
            return String(decoding: try reader.bytes(byteCount), as: UTF8.self)
        }
        var length = Int(try reader.u16())
        if length & 0x8000 != 0 {
            length = ((length & 0x7FFF) << 16) | Int(try reader.u16())
        }
        let bytes = try reader.bytes(length * 2)
        guard try reader.u16() == 0 else { throw APKError.invalidBinaryXML("a string without its NUL") }
        let units = stride(from: 0, to: bytes.count, by: 2).map { UInt16(bytes[$0]) | UInt16(bytes[$0 + 1]) << 8 }
        return String(decoding: units, as: UTF16.self)
    }

    private static func utf8Length(_ reader: inout ByteReader) throws -> Int {
        let first = Int(try reader.u8())
        guard first & 0x80 != 0 else { return first }
        return ((first & 0x7F) << 8) | Int(try reader.u8())
    }
}
