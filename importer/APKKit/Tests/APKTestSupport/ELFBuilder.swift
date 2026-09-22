import Foundation

/// Builds the start of an ELF64 file: header plus program headers (System V gABI layout).
public struct ELFBuilder {
    public struct Segment: Sendable {
        public var type: UInt32
        public var align: UInt64

        public init(type: UInt32, align: UInt64) {
            self.type = type
            self.align = align
        }

        public static func load(align: UInt64) -> Segment { Segment(type: 1, align: align) }
        public static let dynamic = Segment(type: 2, align: 8)
        public static let tls = Segment(type: 7, align: 64)
        public static let relro = Segment(type: 0x6474_E552, align: 1)
    }

    public var elfClass: UInt8 = 2 // ELFCLASS64
    public var byteOrder: UInt8 = 1 // ELFDATA2LSB
    public var machine: UInt16 = 183 // EM_AARCH64
    public var segments: [Segment]

    public init(segments: [Segment]) {
        self.segments = segments
    }

    public func build() -> Data {
        var out = ByteWriter()
        out.bytes([0x7F, 0x45, 0x4C, 0x46, elfClass, byteOrder, 1, 0])
        out.zeros(8)
        out.u16(3) // ET_DYN
        out.u16(machine)
        out.u32(1)
        out.u64(0) // entry
        out.u64(64) // phoff
        out.u64(0) // shoff
        out.u32(0)
        out.u16(64)
        out.u16(56)
        out.u16(UInt16(segments.count))
        out.u16(64)
        out.u16(0)
        out.u16(0)
        for (index, segment) in segments.enumerated() {
            out.u32(segment.type)
            out.u32(segment.type == 1 && index == 0 ? 5 : 6) // R+X, else R+W
            out.u64(0)
            out.u64(UInt64(index) * 0x10000)
            out.u64(UInt64(index) * 0x10000)
            out.u64(0x100)
            out.u64(0x100)
            out.u64(segment.align)
        }
        return out.data
    }
}
