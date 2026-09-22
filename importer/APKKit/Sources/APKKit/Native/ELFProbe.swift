import Foundation

/// Facts about a native library that decide how the loader (M2) can handle it.
public struct ELFInfo: Sendable, Equatable, Codable {
    /// e_machine; 183 is AArch64.
    public var machine: UInt16
    public var loadSegmentCount: Int
    /// Smallest PT_LOAD alignment: 4096 means the library was linked for 4 KB pages only.
    public var pageAlignment: UInt64
    /// The library defines thread-local storage (PT_TLS).
    public var hasTLS: Bool

    public var isAArch64: Bool { machine == 183 }
}

/// Reads the ELF header and program headers of a 64-bit little-endian library.
public enum ELFProbe {
    /// How many bytes from the start of a library `probe` usually needs.
    public static let typicalHeaderSize = 4096

    public static func probe(_ data: Data) throws -> ELFInfo {
        let reader = ByteReader(data, context: "ELF header")
        guard reader.count >= 64, reader.data.prefix(4) == Data([0x7F, 0x45, 0x4C, 0x46]) else {
            throw APKError.invalidELF("missing ELF magic")
        }
        guard reader.data[4] == 2 else { throw APKError.invalidELF("not a 64-bit ELF") }
        guard reader.data[5] == 1 else { throw APKError.invalidELF("not little-endian") }

        let machine = try reader.u16(at: 18)
        let programHeaderOffset = try reader.u64(at: 32)
        let programHeaderSize = Int(try reader.u16(at: 54))
        let programHeaderCount = Int(try reader.u16(at: 56))
        guard programHeaderSize >= 56, programHeaderCount < 0xFFFF,
              programHeaderOffset <= UInt64(reader.count)
        else { throw APKError.invalidELF("malformed program header table") }

        var loadSegments = 0
        var smallestAlignment: UInt64?
        var hasTLS = false
        for index in 0..<programHeaderCount {
            let entry = Int(programHeaderOffset) + index * programHeaderSize
            let type = try reader.u32(at: entry)
            let alignment = try reader.u64(at: entry + 48)
            switch type {
            case 1: // PT_LOAD
                loadSegments += 1
                smallestAlignment = min(smallestAlignment ?? .max, max(alignment, 1))
            case 7: // PT_TLS
                hasTLS = true
            default:
                break
            }
        }
        return ELFInfo(machine: machine, loadSegmentCount: loadSegments, pageAlignment: smallestAlignment ?? 0, hasTLS: hasTLS)
    }
}
