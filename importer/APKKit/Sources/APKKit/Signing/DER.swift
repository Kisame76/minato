import Foundation

/// Minimal ASN.1 DER/BER reader: enough to walk PKCS#7 SignedData and cut out certificates.
struct DERNode {
    let tag: UInt8
    /// Offset of the tag byte; `start..<end` is the complete encoding.
    let start: Int
    let contentStart: Int
    let contentEnd: Int
    let end: Int
}

enum DER {
    private static let maximumDepth = 32

    static func node(in data: Data, at offset: Int, depth: Int = 0) throws -> DERNode {
        guard depth < maximumDepth else { throw APKError.invalidCertificate("nesting too deep") }
        guard offset >= 0, offset + 2 <= data.count else { throw APKError.invalidCertificate("truncated header") }
        let tag = data[offset]
        guard tag & 0x1F != 0x1F else { throw APKError.invalidCertificate("high tag numbers are unsupported") }
        let lengthByte = data[offset + 1]

        if lengthByte == 0x80 {
            // BER indefinite length: constructed content up to an end-of-contents marker (00 00).
            guard tag & 0x20 != 0 else { throw APKError.invalidCertificate("indefinite primitive") }
            var position = offset + 2
            while true {
                guard position + 2 <= data.count else { throw APKError.invalidCertificate("missing end of contents") }
                if data[position] == 0, data[position + 1] == 0 {
                    return DERNode(tag: tag, start: offset, contentStart: offset + 2, contentEnd: position, end: position + 2)
                }
                position = try node(in: data, at: position, depth: depth + 1).end
            }
        }

        var length = Int(lengthByte)
        var contentStart = offset + 2
        if lengthByte & 0x80 != 0 {
            let count = Int(lengthByte & 0x7F)
            guard count <= 4, offset + 2 + count <= data.count else { throw APKError.invalidCertificate("bad length") }
            length = 0
            for index in 0..<count {
                length = length << 8 | Int(data[offset + 2 + index])
            }
            contentStart += count
        }
        guard length <= data.count - contentStart else { throw APKError.invalidCertificate("content beyond end") }
        return DERNode(tag: tag, start: offset, contentStart: contentStart, contentEnd: contentStart + length, end: contentStart + length)
    }

    static func children(of parent: DERNode, in data: Data, depth: Int = 0) throws -> [DERNode] {
        var result: [DERNode] = []
        var position = parent.contentStart
        while position < parent.contentEnd {
            let child = try node(in: data, at: position, depth: depth + 1)
            result.append(child)
            position = child.end
        }
        return result
    }
}

extension DER {
    /// An OBJECT IDENTIFIER's value in dotted form, e.g. "1.2.840.113549.1.7.2".
    static func objectIdentifier(_ node: DERNode, in data: Data) throws -> String {
        guard node.tag == 0x06, node.contentEnd > node.contentStart else { throw APKError.invalidCertificate("not an OID") }
        var arcs: [UInt64] = []
        var value: UInt64 = 0
        for index in node.contentStart..<node.contentEnd {
            guard value >> 56 == 0 else { throw APKError.invalidCertificate("OID arc too large") }
            value = value << 7 | UInt64(data[index] & 0x7F)
            if data[index] & 0x80 == 0 {
                if arcs.isEmpty {
                    arcs = value < 80 ? [value / 40, value % 40] : [2, value - 80]
                } else {
                    arcs.append(value)
                }
                value = 0
            }
        }
        guard data[node.contentEnd - 1] & 0x80 == 0 else { throw APKError.invalidCertificate("truncated OID") }
        return arcs.map(String.init).joined(separator: ".")
    }

    static func bytes(of node: DERNode, in data: Data) -> Data {
        data.subdata(in: node.start..<node.end)
    }

    /// An element of `tag` around `content`, its length in DER's shortest form.
    static func encode(_ tag: UInt8, _ content: Data) -> Data {
        var length: [UInt8] = []
        var rest = content.count
        while rest > 0 {
            length.insert(UInt8(rest & 0xFF), at: 0)
            rest >>= 8
        }
        let header = content.count < 0x80 ? [UInt8(content.count)] : [0x80 | UInt8(length.count)] + length
        return Data([tag] + header) + content
    }

    static func content(of node: DERNode, in data: Data) -> Data {
        data.subdata(in: node.contentStart..<node.contentEnd)
    }
}

enum PKCS7 {
    /// DER encodings of the certificates in a PKCS#7 SignedData (ContentInfo), e.g. META-INF/CERT.RSA.
    static func certificates(in encoded: Data) throws -> [Data] {
        let data = Data(encoded)
        let contentInfo = try DER.node(in: data, at: 0)
        let parts = try DER.children(of: contentInfo, in: data)
        guard contentInfo.tag == 0x30, parts.count >= 2, parts[0].tag == 0x06, parts[1].tag == 0xA0,
              let signedData = try DER.children(of: parts[1], in: data).first, signedData.tag == 0x30
        else { throw APKError.invalidCertificate("not a PKCS#7 SignedData") }

        // certificates [0] IMPLICIT SET OF Certificate
        guard let certificateSet = try DER.children(of: signedData, in: data).first(where: { $0.tag == 0xA0 }) else {
            return []
        }
        return try DER.children(of: certificateSet, in: data)
            .filter { $0.tag == 0x30 }
            .map { data.subdata(in: $0.start..<$0.end) }
    }
}
