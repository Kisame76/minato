import APKFormats
import APKTestSupport
import Foundation

/// A ZIP written straight to disk, every entry stored, whose runs of zeros stay holes of a sparse file, so a test of
/// an OBB over 4 GiB costs neither memory nor disk. A size or an offset from 4 GiB on takes ZIP64's extra field, and a
/// central directory from there ZIP64's end records (APPNOTE 4.3.14 to 4.3.15, 4.5.3).
enum SparseZIP {
    enum Part {
        case bytes(Data)
        case zeros(UInt64)

        var count: UInt64 {
            switch self {
            case .bytes(let data): UInt64(data.count)
            case .zeros(let count): count
            }
        }
    }

    static func write(_ entries: [(name: String, parts: [Part])], to url: URL) throws {
        guard FileManager.default.createFile(atPath: url.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: url.path])
        }
        let handle = try FileHandle(forWritingTo: url)
        defer { try? handle.close() }
        var offset: UInt64 = 0
        var directory = ByteWriter()
        for (name, parts) in entries {
            let size = parts.reduce(0) { $0 + $1.count }
            let wideSize = size >= 0xFFFF_FFFF, wideOffset = offset >= 0xFFFF_FFFF
            let fields = Fields(name: Data(name.utf8), crc: parts.reduce(UInt32(0), checksum),
                                size32: wideSize ? .max : UInt32(size), version: wideSize || wideOffset ? 45 : 20)
            var local = ByteWriter()
            local.u32(0x0403_4b50)
            local.u16(fields.version)
            fields.write(to: &local, extra: zip64(wideSize ? [size, size] : []))
            directory.u32(0x0201_4b50)
            directory.u16(fields.version)
            directory.u16(fields.version)
            let extra = zip64((wideSize ? [size, size] : []) + (wideOffset ? [offset] : []))
            fields.write(to: &directory, extra: extra, offset: wideOffset ? .max : UInt32(offset))
            try handle.seek(toOffset: offset)
            try handle.write(contentsOf: local.data)
            offset += UInt64(local.count)
            for part in parts {
                if case .bytes(let data) = part {
                    try handle.seek(toOffset: offset)
                    try handle.write(contentsOf: data)
                }
                offset += part.count
            }
        }
        let directoryOffset = offset, count = UInt64(entries.count), size = UInt64(directory.count)
        var end = ByteWriter()
        let wide = directoryOffset >= 0xFFFF_FFFF || count >= 0xFFFF
        if wide {
            end.u32(0x0606_4b50); end.u64(44); end.u16(45); end.u16(45); end.u32(0); end.u32(0)
            end.u64(count); end.u64(count); end.u64(size); end.u64(directoryOffset)
            end.u32(0x0706_4b50); end.u32(0); end.u64(directoryOffset + size); end.u32(1)
        }
        end.u32(0x0605_4b50); end.u16(0); end.u16(0)
        end.u16(wide ? .max : UInt16(count)); end.u16(wide ? .max : UInt16(count))
        end.u32(UInt32(size)); end.u32(wide ? .max : UInt32(directoryOffset)); end.u16(0)
        try handle.seek(toOffset: directoryOffset)
        try handle.write(contentsOf: directory.data + end.data)
    }

    /// The fields a local header and a central directory record share, from the flags on.
    private struct Fields {
        var name: Data
        var crc: UInt32
        var size32: UInt32
        var version: UInt16

        /// A central directory record's fields after the name's length when `offset` is given.
        func write(to out: inout ByteWriter, extra: Data, offset: UInt32? = nil) {
            out.u16(0); out.u16(0); out.u16(0); out.u16(0x21)
            out.u32(crc); out.u32(size32); out.u32(size32)
            out.u16(UInt16(name.count)); out.u16(UInt16(extra.count))
            if let offset {
                out.u16(0); out.u16(0); out.u16(0); out.u32(0); out.u32(offset)
            }
            out.bytes(name)
            out.bytes(extra)
        }
    }

    /// The running CRC-32 `crc` with `part` added, a run of zeros one zeroed megabyte at a time.
    static func checksum(_ crc: UInt32, _ part: Part) -> UInt32 {
        switch part {
        case .bytes(let data):
            return data.withUnsafeBytes { Container.checksum(crc, adding: $0) }
        case .zeros(let count):
            var crc = crc, left = count
            Data(count: 1 << 20).withUnsafeBytes { zeros in
                while left > 0 {
                    let step = Int(min(left, UInt64(zeros.count)))
                    crc = Container.checksum(crc, adding: UnsafeRawBufferPointer(rebasing: zeros[0..<step]))
                    left -= UInt64(step)
                }
            }
            return crc
        }
    }

    /// The bytes a file takes on its volume: its holes take none.
    static func allocated(_ url: URL) throws -> UInt64 {
        var info = stat()
        guard stat(url.path, &info) == 0 else {
            throw CocoaError(.fileReadNoSuchFile, userInfo: [NSFilePathErrorKey: url.path])
        }
        return UInt64(info.st_blocks) * 512
    }

    /// ZIP64's extended information field with `values`, none for no values.
    private static func zip64(_ values: [UInt64]) -> Data {
        guard !values.isEmpty else { return Data() }
        var field = ByteWriter()
        field.u16(0x0001)
        field.u16(UInt16(values.count * 8))
        for value in values { field.u64(value) }
        return field.data
    }
}
