import APKPack
import Foundation

/// A small AArch64 shared object with a dynamic section that names `needed` libraries (DT_NEEDED), as far as the
/// reader of NEEDED names needs: one PT_LOAD for the whole file at an address 0x1000 above the file offsets, so that
/// DT_STRTAB's address has to be mapped back, and a PT_DYNAMIC.
public enum DynamicELF {
    public static func make(needed: [String], machine: UInt16 = 183) -> Data {
        var names = Data([0])
        var offsets: [UInt64] = []
        for name in needed {
            offsets.append(UInt64(names.count))
            names += Data(name.utf8) + Data([0])
        }
        let headerSize = 64 + 2 * 56
        let entries = offsets.count + 3  // the names, DT_STRTAB, DT_STRSZ, DT_NULL
        let dynamicOffset = headerSize
        let stringsOffset = dynamicOffset + entries * 16
        let total = stringsOffset + names.count
        var out = ByteWriter()
        out.bytes([0x7F, 0x45, 0x4C, 0x46, 2, 1, 1, 0])
        out.zeros(8)
        out.u16(3)  // ET_DYN
        out.u16(machine)
        out.u32(1)
        out.u64(0)
        out.u64(64)  // phoff
        out.u64(0)
        out.u32(0)
        out.u16(64)
        out.u16(56)
        out.u16(2)
        out.u16(64)
        out.u16(0)
        out.u16(0)
        // PT_LOAD: the whole file, at 0x1000 + offset.
        out.u32(1); out.u32(5); out.u64(0); out.u64(0x1000); out.u64(0x1000); out.u64(UInt64(total))
        out.u64(UInt64(total)); out.u64(0x4000)
        // PT_DYNAMIC
        out.u32(2); out.u32(6); out.u64(UInt64(dynamicOffset)); out.u64(0x1000 + UInt64(dynamicOffset))
        out.u64(0x1000 + UInt64(dynamicOffset))
        out.u64(UInt64(entries * 16)); out.u64(UInt64(entries * 16)); out.u64(8)
        for offset in offsets { out.u64(1); out.u64(offset) }  // DT_NEEDED
        out.u64(5); out.u64(0x1000 + UInt64(stringsOffset))  // DT_STRTAB
        out.u64(10); out.u64(UInt64(names.count))  // DT_STRSZ
        out.u64(0); out.u64(0)  // DT_NULL
        out.bytes(Array(names))
        return out.data
    }
}
