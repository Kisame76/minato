import Foundation

/// Writes the ASN.1 DER that test signatures need (PKCS#7 SignedData for v1), and cuts the few fields
/// they need out of a certificate. Independent of APKKit's reader, so a test does not check the reader
/// against itself.
public enum DERWriter {
    public static func tagged(_ tag: UInt8, _ content: Data) -> Data {
        var out = Data([tag])
        if content.count < 0x80 {
            out.append(UInt8(content.count))
        } else {
            var length = content.count
            var bytes: [UInt8] = []
            while length > 0 {
                bytes.insert(UInt8(length & 0xFF), at: 0)
                length >>= 8
            }
            out.append(0x80 | UInt8(bytes.count))
            out.append(contentsOf: bytes)
        }
        return out + content
    }

    public static func sequence(_ parts: [Data]) -> Data { tagged(0x30, parts.reduce(Data(), +)) }
    public static func set(_ parts: [Data]) -> Data { tagged(0x31, parts.reduce(Data(), +)) }
    public static func octetString(_ bytes: Data) -> Data { tagged(0x04, bytes) }
    public static let null = Data([0x05, 0x00])

    public static func integer(_ value: Int) -> Data {
        var bytes: [UInt8] = []
        var rest = value
        repeat {
            bytes.insert(UInt8(rest & 0xFF), at: 0)
            rest >>= 8
        } while rest > 0
        if bytes[0] & 0x80 != 0 { bytes.insert(0, at: 0) }
        return tagged(0x02, Data(bytes))
    }

    /// An object identifier in dotted form, e.g. "1.2.840.113549.1.7.2".
    public static func oid(_ dotted: String) -> Data {
        let arcs = dotted.split(separator: ".").map { Int($0)! }
        var body = Data([UInt8(arcs[0] * 40 + arcs[1])])
        for arc in arcs.dropFirst(2) {
            var chunk: [UInt8] = [UInt8(arc & 0x7F)]
            var rest = arc >> 7
            while rest > 0 {
                chunk.insert(UInt8(rest & 0x7F) | 0x80, at: 0)
                rest >>= 7
            }
            body.append(contentsOf: chunk)
        }
        return tagged(0x06, body)
    }

    /// An AlgorithmIdentifier: the OID, with NULL parameters unless `parameters` is false.
    public static func algorithm(_ dotted: String, parameters: Bool = true) -> Data {
        sequence(parameters ? [oid(dotted), null] : [oid(dotted)])
    }

    /// The complete encodings of a constructed element's children.
    public static func children(of element: Data) -> [Data] {
        let data = Data(element)
        var (position, end) = header(data, at: 0)
        end = min(end, data.count)
        var result: [Data] = []
        while position < end {
            let elementEnd = header(data, at: position).end
            result.append(data.subdata(in: position..<elementEnd))
            position = elementEnd
        }
        return result
    }

    /// An element's content, without its tag and length.
    public static func content(of element: Data) -> Data {
        let data = Data(element)
        let (start, end) = header(data, at: 0)
        return data.subdata(in: start..<end)
    }

    /// Where the content of the element at `offset` starts and where the element ends.
    private static func header(_ data: Data, at offset: Int) -> (contentStart: Int, end: Int) {
        let first = data[offset + 1]
        guard first & 0x80 != 0 else { return (offset + 2, offset + 2 + Int(first)) }
        let count = Int(first & 0x7F)
        var length = 0
        for index in 0..<count { length = length << 8 | Int(data[offset + 2 + index]) }
        return (offset + 2 + count, offset + 2 + count + length)
    }

    /// The issuer and the serial number of a certificate, as PKCS#7's IssuerAndSerialNumber names it.
    public static func issuerAndSerialNumber(of certificate: Data) -> Data {
        let tbs = children(of: children(of: certificate)[0])
        let offset = tbs[0].first == 0xA0 ? 1 : 0
        // TBSCertificate: [0] version (optional), serialNumber, signature, issuer, ...
        return sequence([tbs[offset + 2], tbs[offset]])
    }

    /// A certificate's SubjectPublicKeyInfo, exactly as it stands in the certificate.
    public static func subjectPublicKeyInfo(of certificate: Data) -> Data {
        let tbs = children(of: children(of: certificate)[0])
        return tbs[(tbs[0].first == 0xA0 ? 1 : 0) + 5]
    }
}
