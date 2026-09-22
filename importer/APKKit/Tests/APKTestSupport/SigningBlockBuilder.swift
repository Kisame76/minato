import Foundation

/// Builds APK Signing Blocks (APK Signature Scheme v2/v3, source stamp v2) for tests.
/// Digests and signatures are placeholders: the tests cover certificate extraction, not verification.
public enum SigningBlockBuilder {
    public static let v2ID: UInt32 = 0x7109_871A
    public static let v3ID: UInt32 = 0xF053_68C0
    public static let v31ID: UInt32 = 0x1B93_AD61
    public static let sourceStampV2ID: UInt32 = 0x6DFF_800D

    public static func v2Signer(certificates: [Data]) -> Data {
        var signedData = ByteWriter()
        signedData.bytes(prefixed(placeholderDigests()))
        signedData.bytes(prefixed(certificates.map(prefixed).reduce(Data(), +)))
        signedData.bytes(prefixed(Data()))

        var signer = ByteWriter()
        signer.bytes(prefixed(signedData.data))
        signer.bytes(prefixed(placeholderSignatures()))
        signer.bytes(prefixed(Data(repeating: 0x42, count: 16)))
        return signer.data
    }

    public static func v3Signer(certificates: [Data], minSdk: UInt32 = 24, maxSdk: UInt32 = 0x7FFF_FFFF) -> Data {
        var signedData = ByteWriter()
        signedData.bytes(prefixed(placeholderDigests()))
        signedData.bytes(prefixed(certificates.map(prefixed).reduce(Data(), +)))
        signedData.u32(minSdk)
        signedData.u32(maxSdk)
        signedData.bytes(prefixed(Data()))

        var signer = ByteWriter()
        signer.bytes(prefixed(signedData.data))
        signer.u32(minSdk)
        signer.u32(maxSdk)
        signer.bytes(prefixed(placeholderSignatures()))
        signer.bytes(prefixed(Data(repeating: 0x42, count: 16)))
        return signer.data
    }

    /// Value of a v2/v3/v3.1 block: a length-prefixed sequence of length-prefixed signers.
    public static func schemeValue(_ signers: [Data]) -> Data {
        prefixed(signers.map(prefixed).reduce(Data(), +))
    }

    public static func sourceStampValue(certificate: Data) -> Data {
        var stamp = ByteWriter()
        stamp.bytes(prefixed(certificate))
        stamp.bytes(prefixed(Data()))
        stamp.bytes(prefixed(Data()))
        stamp.bytes(prefixed(Data()))
        return prefixed(stamp.data)
    }

    public static func block(_ pairs: [(id: UInt32, value: Data)]) -> Data {
        var body = ByteWriter()
        for pair in pairs {
            body.u64(UInt64(4 + pair.value.count))
            body.u32(pair.id)
            body.bytes(pair.value)
        }
        let size = UInt64(body.count + 8 + 16)
        var block = ByteWriter()
        block.u64(size)
        block.bytes(body.data)
        block.u64(size)
        block.bytes(Data("APK Sig Block 42".utf8))
        return block.data
    }

    public static func prefixed(_ data: Data) -> Data {
        var writer = ByteWriter()
        writer.u32(UInt32(data.count))
        writer.bytes(data)
        return writer.data
    }

    private static func placeholderDigests() -> Data {
        var record = ByteWriter()
        record.u32(0x0103)
        record.bytes(prefixed(Data(repeating: 0xD1, count: 32)))
        return prefixed(record.data)
    }

    private static func placeholderSignatures() -> Data {
        var record = ByteWriter()
        record.u32(0x0103)
        record.bytes(prefixed(Data(repeating: 0x5A, count: 64)))
        return prefixed(record.data)
    }
}
