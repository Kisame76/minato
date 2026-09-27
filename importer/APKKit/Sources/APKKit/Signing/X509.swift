import Foundation

/// The parts of an X.509 certificate (RFC 5280) that signing and its checks need.
public enum X509 {
    /// The DER encoding of the certificate's SubjectPublicKeyInfo, exactly as it stands in the
    /// certificate: APK Signature Scheme v2 requires the signer's public key to be these bytes.
    public static func subjectPublicKeyInfo(of certificate: Data) throws -> Data {
        let data = Data(certificate)
        let root = try DER.node(in: data, at: 0)
        guard root.tag == 0x30, let tbs = try DER.children(of: root, in: data).first, tbs.tag == 0x30 else {
            throw APKError.invalidCertificate("not an X.509 certificate")
        }
        // TBSCertificate: [0] version (optional), serialNumber, signature, issuer, validity, subject,
        // subjectPublicKeyInfo, ...
        let fields = try DER.children(of: tbs, in: data)
        let index = (fields.first?.tag == 0xA0 ? 1 : 0) + 5
        guard index < fields.count, fields[index].tag == 0x30 else {
            throw APKError.invalidCertificate("no SubjectPublicKeyInfo")
        }
        return data.subdata(in: fields[index].start..<fields[index].end)
    }

    /// The DER encodings of the certificate's issuer and serial number, which PKCS#7 names a signer by.
    static func issuerAndSerialNumber(of certificate: Data) throws -> (issuer: Data, serial: Data) {
        let data = Data(certificate)
        let root = try DER.node(in: data, at: 0)
        guard root.tag == 0x30, let tbs = try DER.children(of: root, in: data).first, tbs.tag == 0x30 else {
            throw APKError.invalidCertificate("not an X.509 certificate")
        }
        let fields = try DER.children(of: tbs, in: data)
        let first = fields.first?.tag == 0xA0 ? 1 : 0
        guard first + 2 < fields.count else { throw APKError.invalidCertificate("no issuer") }
        return (data.subdata(in: fields[first + 2].start..<fields[first + 2].end),
                data.subdata(in: fields[first].start..<fields[first].end))
    }
}
