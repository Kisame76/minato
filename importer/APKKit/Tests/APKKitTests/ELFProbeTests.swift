import APKTestSupport
import Foundation
import Testing

@testable import APKKit

@Suite("ELFProbe")
struct ELFProbeTests {
    @Test func reads16KAlignedAArch64Library() throws {
        let elf = ELFBuilder(segments: [.load(align: 0x4000), .load(align: 0x4000), .dynamic, .tls, .relro]).build()
        let info = try ELFProbe.probe(elf)

        #expect(info.isAArch64)
        #expect(info.loadSegmentCount == 2)
        #expect(info.pageAlignment == 16_384)
        #expect(info.hasTLS)
    }

    @Test func reports4KAlignment() throws {
        let info = try ELFProbe.probe(ELFBuilder(segments: [.load(align: 0x1000), .load(align: 0x1000), .dynamic]).build())

        #expect(info.pageAlignment == 4_096)
        #expect(!info.hasTLS)
    }

    @Test func reportsSmallestAlignmentOfMixedSegments() throws {
        let info = try ELFProbe.probe(ELFBuilder(segments: [.load(align: 0x4000), .load(align: 0x1000), .load(align: 0x10000)]).build())

        #expect(info.loadSegmentCount == 3)
        #expect(info.pageAlignment == 4_096)
    }

    @Test func recognisesOtherMachines() throws {
        var builder = ELFBuilder(segments: [.load(align: 0x1000)])
        builder.machine = 62 // x86-64 library in an arm64 folder
        let info = try ELFProbe.probe(builder.build())

        #expect(!info.isAArch64)
        #expect(info.machine == 62)
    }

    @Test func rejects32BitAndBigEndianFiles() {
        var thirtyTwo = ELFBuilder(segments: [.load(align: 0x1000)])
        thirtyTwo.elfClass = 1
        var bigEndian = ELFBuilder(segments: [.load(align: 0x1000)])
        bigEndian.byteOrder = 2

        #expect(throws: APKError.self) { _ = try ELFProbe.probe(thirtyTwo.build()) }
        #expect(throws: APKError.self) { _ = try ELFProbe.probe(bigEndian.build()) }
    }

    @Test func rejectsFilesThatAreNotELF() {
        #expect(throws: APKError.self) { _ = try ELFProbe.probe(Data("not an elf file at all, just text".utf8)) }
        #expect(throws: APKError.self) { _ = try ELFProbe.probe(Data()) }
    }

    @Test func rejectsTruncatedProgramHeaders() {
        let elf = ELFBuilder(segments: [.load(align: 0x4000), .load(align: 0x4000)]).build()

        #expect(throws: APKError.self) { _ = try ELFProbe.probe(elf.prefix(64 + 56 + 10)) }
    }

    @Test func survivesCorruptedHeaders() {
        let valid = ELFBuilder(segments: [.load(align: 0x4000), .load(align: 0x4000), .dynamic, .tls]).build()
        var generator = SplitMix64(seed: 0xE1F)
        for _ in 0..<2_000 {
            var corrupted = valid
            let position = Int.random(in: 0..<corrupted.count, using: &generator)
            corrupted[position] = UInt8.random(in: 0...255, using: &generator)
            _ = try? ELFProbe.probe(corrupted)
        }
    }
}
