import CryptoKit
import Foundation
import Security

/// Verifies an APK's signatures cryptographically, as Android 15 does (ADR-0034, step d): the newest
/// scheme with a signer for Android 15 decides, v3.1, then v3, then v2, then v1 (decision 2). Its
/// signatures are checked with Security.framework and the content it covers is digested again, one
/// piece at a time, so memory stays bounded for any size.
public enum SignatureVerifier {
    static let v2ID: UInt32 = 0x7109_871A
    static let v3ID: UInt32 = 0xF053_68C0
    static let v31ID: UInt32 = 0x1B93_AD61
    /// v2 signed data's attribute naming the newer scheme the APK is also signed with.
    static let strippingProtectionID: UInt32 = 0xBEEF_F00D
    /// v3 signed data's attribute saying that a v3.1 block carries the rotated key.
    static let rotationMinSdkID: UInt32 = 0x559F_8B02
    static let proofOfRotationID: UInt32 = 0x3BA0_6F8C
    /// v3.1 signed data's attribute saying that the signer is for the development preview of its minSdk.
    static let rotationOnDevReleaseID: UInt32 = 0xC2A6_B3BA
    /// Android 15's SDK version, which ApkSignatureSchemeV3Verifier compares as an int.
    static let platformSDK = Int32(APKSignatures.platformSDK)
    static let chunkSize = 1 << 20

    /// The check of `archive`, whose manifest names `targetSdk`. A proof that signers.json cannot hold leaves the
    /// file unverified: the guest would not see that signer (ADR-0034 decision 2).
    public static func verify(_ archive: ZipArchive, targetSdk: Int) -> SignatureVerification {
        let verification: SignatureVerification
        do {
            verification = try check(archive, targetSdk: targetSdk)
        } catch let failure as SignatureFailure {
            return SignatureVerification(check: .failed(failure))
        } catch {
            return SignatureVerification(check: .failed(SignatureFailure(.damaged, "\(error)")))
        }
        if let proof = verification.proof, let problem = SignersFile.problem(with: proof) {
            return SignatureVerification(check: .unverified(problem))
        }
        return verification
    }

    static func check(_ archive: ZipArchive, targetSdk: Int) throws -> SignatureVerification {
        let block: (offset: UInt64, pairs: [(UInt32, Data)])?
        do {
            block = try APKSignatures.signingBlock(archive)
        } catch {
            throw SignatureFailure(.damaged, "the signing block: \(error)")
        }
        var schemes: [UInt32: Data] = [:]
        for (id, value) in block?.pairs ?? [] where [v2ID, v3ID, v31ID].contains(id) {
            // The certificates of both would count, and Android verifies only the first.
            guard schemes[id] == nil else { throw SignatureFailure(.damaged, "two \(name(id)) blocks") }
            schemes[id] = value
        }
        if let block {
            // What v3.1 leaves for v3's rotation-min-sdk attribute (ApkSignatureSchemeV3Verifier's
            // mOptionalRotationMinSdkVersion).
            var rotationMinSdk: Int32?
            for id in [v31ID, v3ID, v2ID] {
                guard let value = schemes[id] else { continue }
                if let result = try verifyScheme(id, value, archive: archive, blockOffset: block.offset,
                                                 present: Set(schemes.keys), rotationMinSdk: &rotationMinSdk) {
                    return result
                }
            }
        }
        // From targetSdkVersion 30 on, Android needs v2 or newer and does not look at v1
        // (ApkSignatureVerifier.java:218-226 and 572-577).
        if targetSdk >= 30, archive.entries.contains(where: { APKSignatures.isV1SignatureFile($0.name) }) {
            throw SignatureFailure(.noV2Signature,
                                   "No APK Signature Scheme v2 signature: only v1, which targetSdkVersion \(targetSdk) does not allow")
        }
        return try JarVerifier.verify(archive, present: Set(schemes.keys))
    }

    static func name(_ id: UInt32) -> String {
        switch id {
        case v2ID: "v2"
        case v3ID: "v3"
        default: "v3.1"
        }
    }

    // MARK: - Algorithms

    enum ContentDigest: Int, Comparable {
        case sha256, sha512

        static func < (lhs: ContentDigest, rhs: ContentDigest) -> Bool { lhs.rawValue < rhs.rawValue }
    }

    struct Algorithm {
        /// Nil for an algorithm Android knows and Security.framework cannot check.
        var secAlgorithm: SecKeyAlgorithm?
        var digest: ContentDigest?
        var name: String
    }

    static func algorithm(_ id: UInt32) -> Algorithm? {
        switch id {
        case 0x0101: Algorithm(secAlgorithm: .rsaSignatureMessagePSSSHA256, digest: .sha256, name: "RSASSA-PSS with SHA-256")
        case 0x0102: Algorithm(secAlgorithm: .rsaSignatureMessagePSSSHA512, digest: .sha512, name: "RSASSA-PSS with SHA-512")
        case 0x0103: Algorithm(secAlgorithm: .rsaSignatureMessagePKCS1v15SHA256, digest: .sha256, name: "RSASSA-PKCS1-v1_5 with SHA-256")
        case 0x0104: Algorithm(secAlgorithm: .rsaSignatureMessagePKCS1v15SHA512, digest: .sha512, name: "RSASSA-PKCS1-v1_5 with SHA-512")
        case 0x0201: Algorithm(secAlgorithm: .ecdsaSignatureMessageX962SHA256, digest: .sha256, name: "ECDSA with SHA-256")
        case 0x0202: Algorithm(secAlgorithm: .ecdsaSignatureMessageX962SHA512, digest: .sha512, name: "ECDSA with SHA-512")
        case 0x0301: Algorithm(name: "DSA with SHA-256")
        case 0x0421: Algorithm(name: "RSASSA-PKCS1-v1_5 over a verity digest")
        case 0x0423: Algorithm(name: "ECDSA over a verity digest")
        case 0x0425: Algorithm(name: "DSA over a verity digest")
        default: nil
        }
    }

    static let rsaOID = "1.2.840.113549.1.1.1"
    static let ecOID = "1.2.840.10045.2.1"
    /// P-256, P-384 and P-521, the curves Security.framework has.
    static let curveOIDs: Set<String> = ["1.2.840.10045.3.1.7", "1.3.132.0.34", "1.3.132.0.35"]

    /// The certificate's public key, cut out of its SubjectPublicKeyInfo.
    static func publicKey(of certificate: Data) throws -> SecKey {
        let spki: Data
        do {
            spki = try X509.subjectPublicKeyInfo(of: certificate)
        } catch {
            throw SignatureFailure(.signatureMismatch, "a signer's certificate cannot be read")
        }
        return try publicKey(subjectPublicKeyInfo: spki)
    }

    /// An RSA key or an EC key on a NIST curve. Another type, such as DSA or a curve Security.framework
    /// lacks, or a key it cannot read, leaves the file unverified, like an algorithm it cannot check.
    static func publicKey(subjectPublicKeyInfo spki: Data) throws -> SecKey {
        try readKey(spki).key
    }

    /// The key as Java's getEncoded writes it, which Android compares byte for byte with a v2 or v3
    /// signer's public key: NULL parameters for RSA, the named curve for EC, and the key as
    /// Security.framework writes it, so an RSA modulus stored as a negative INTEGER comes out positive.
    static func javaEncoding(ofKeyIn certificate: Data) throws -> Data {
        let spki: Data
        do {
            spki = try X509.subjectPublicKeyInfo(of: certificate)
        } catch {
            throw SignatureFailure(.signatureMismatch, "a signer's certificate cannot be read")
        }
        let (key, algorithm) = try readKey(spki)
        guard let encoded = SecKeyCopyExternalRepresentation(key, nil) as Data? else {
            throw Unverifiable(reason: "a key Security.framework cannot write")
        }
        return DER.encode(0x30, algorithm + DER.encode(0x03, Data([0]) + encoded))
    }

    /// A PKCS #1 RSA key whose modulus, stored as a negative INTEGER as old Java runtimes wrote it, is
    /// read as unsigned, as apksig reads it; Security.framework refuses the negative one.
    static func unsignedModulus(_ key: Data) throws -> Data {
        let parts: [DERNode]
        do {
            parts = try DER.children(of: try DER.node(in: key, at: 0), in: key)
        } catch {
            throw SignatureFailure(.signatureMismatch, "an RSA key cannot be read")
        }
        guard parts.count == 2, parts[0].tag == 0x02, let first = DER.content(of: parts[0], in: key).first, first & 0x80 != 0 else {
            return key
        }
        return DER.encode(0x30, DER.encode(0x02, Data([0]) + DER.content(of: parts[0], in: key)) + DER.bytes(of: parts[1], in: key))
    }

    /// The key, and its AlgorithmIdentifier as Java writes it.
    static func readKey(_ spki: Data) throws -> (key: SecKey, algorithm: Data) {
        let data = Data(spki)
        let parsed: (algorithm: String, parameter: String?, curve: Data, key: Data)
        do {
            let root = try DER.node(in: data, at: 0)
            let parts = try DER.children(of: root, in: data)
            guard root.tag == 0x30, parts.count == 2, parts[0].tag == 0x30, parts[1].tag == 0x03 else {
                throw SignatureFailure(.signatureMismatch, "a signer's public key cannot be read")
            }
            let algorithm = try DER.children(of: parts[0], in: data)
            guard let oid = algorithm.first else { throw SignatureFailure(.signatureMismatch, "a public key without its algorithm") }
            let named = algorithm.count > 1 && algorithm[1].tag == 0x06
            let parameter = named ? try DER.objectIdentifier(algorithm[1], in: data) : nil
            // BIT STRING: the number of unused bits, then the key.
            let bits = DER.content(of: parts[1], in: data)
            guard bits.first == 0 else { throw SignatureFailure(.signatureMismatch, "a public key of partial bytes") }
            parsed = (try DER.objectIdentifier(oid, in: data), parameter, named ? DER.bytes(of: algorithm[1], in: data) : Data(),
                      bits.dropFirst())
        } catch let failure as SignatureFailure {
            throw failure
        } catch {
            throw SignatureFailure(.signatureMismatch, "a signer's public key cannot be read: \(error)")
        }
        let type: CFString
        let algorithm: Data
        switch parsed.algorithm {
        case rsaOID:
            type = kSecAttrKeyTypeRSA
            algorithm = DER.encode(0x30, DER.encode(0x06, Data([0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01])) + Data([0x05, 0x00]))
        case ecOID where parsed.parameter.map(curveOIDs.contains) == true:
            type = kSecAttrKeyTypeECSECPrimeRandom
            algorithm = DER.encode(0x30, DER.encode(0x06, Data([0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01])) + parsed.curve)
        default:
            throw Unverifiable(reason: "a key of type \(parsed.algorithm)\(parsed.parameter.map { " (\($0))" } ?? "")")
        }
        var encoded = Data(parsed.key)
        if type == kSecAttrKeyTypeRSA { encoded = try unsignedModulus(encoded) }
        let attributes = [kSecAttrKeyType: type, kSecAttrKeyClass: kSecAttrKeyClassPublic] as CFDictionary
        guard let key = SecKeyCreateWithData(encoded as CFData, attributes, nil) else {
            throw Unverifiable(reason: "a key Security.framework cannot read")
        }
        return (key, algorithm)
    }

    static func verifies(_ key: SecKey, _ algorithm: SecKeyAlgorithm, _ message: Data, _ signature: Data) -> Bool {
        SecKeyIsAlgorithmSupported(key, .verify, algorithm)
            && SecKeyVerifySignature(key, algorithm, message as CFData, signature as CFData, nil)
    }

    // MARK: - v2, v3, v3.1

    struct Signer {
        var signedData = Data()
        var digests: [(algorithm: UInt32, value: Data)] = []
        var certificates: [Data] = []
        var signedMinSdk: UInt32 = 0
        var signedMaxSdk: UInt32 = 0
        var attributes: [(id: UInt32, value: Data)] = []
        var minSdk: UInt32 = 0
        var maxSdk: UInt32 = .max
        var signatures: [(algorithm: UInt32, value: Data)] = []
        var publicKey = Data()
    }

    /// The scheme's verification; nil for v3.1 without a signer for Android 15, so that v3 or an older scheme
    /// decides (ApkSignatureSchemeV3Verifier.java:136-155).
    static func verifyScheme(_ id: UInt32, _ value: Data, archive: ZipArchive, blockOffset: UInt64,
                             present: Set<UInt32>, rotationMinSdk: inout Int32?) throws -> SignatureVerification? {
        let isV3 = id != v2ID
        let signers: [Signer]
        do {
            var outer = ByteReader(value, context: name(id))
            var list = ByteReader(try prefixed(&outer), context: "\(name(id)) signers")
            var parsed: [Signer] = []
            // One past the limit is enough to refuse the block.
            while list.remaining > 0, parsed.count <= JarVerifier.maximumSigners {
                parsed.append(try signer(try prefixed(&list), isV3: isV3))
            }
            signers = parsed
        } catch {
            throw SignatureFailure(.damaged, "the \(name(id)) block: \(error)")
        }
        guard !signers.isEmpty else { throw SignatureFailure(.signatureMismatch, "the \(name(id)) block has no signer") }
        // As Android: no more than 10 signers.
        guard signers.count <= JarVerifier.maximumSigners else {
            throw SignatureFailure(.signatureMismatch, "more than \(JarVerifier.maximumSigners) \(name(id)) signers")
        }
        var unverifiable: String?
        var contentDigests: [ContentDigest: Data] = [:]
        // The signers that count, in the block's order, each with its verified lineage.
        var counted: [(certificate: Data, lineage: [SignerProof.Level])] = []
        for signer in signers {
            // A v3 signer for other Android versions is skipped before its signature is checked.
            if isV3, !isForAndroid15(signer, scheme: id, rotationMinSdk: &rotationMinSdk) { continue }
            do {
                let (digest, value) = try verifySigner(signer, scheme: id, present: present)
                if let known = contentDigests[digest], known != value {
                    throw SignatureFailure(.damaged, "the \(name(id)) signers' content digests differ")
                }
                contentDigests[digest] = value
            } catch let reason as Unverifiable {
                unverifiable = reason.reason
            }
            var lineage: [SignerProof.Level] = []
            if isV3 {
                do {
                    // nil: a v3.1 signer for Android 15's development preview, which the release skips.
                    guard let verified = try verifyAttributes(of: signer, scheme: id, rotationMinSdk: &rotationMinSdk) else {
                        continue
                    }
                    lineage = verified
                } catch let reason as Unverifiable {
                    unverifiable = reason.reason
                }
            }
            counted.append((signer.certificates[0], lineage))
        }
        if isV3 {
            // As ApkSignatureSchemeV3Verifier.java:233-245: v3.1 without a signer for Android 15 falls back to v3,
            // v3 without one fails, and one signer at most.
            if counted.isEmpty, id == v31ID { return nil }
            guard !counted.isEmpty else { throw SignatureFailure(.signatureMismatch, "no v3 signer for Android 15") }
            guard counted.count == 1 else {
                throw SignatureFailure(.signatureMismatch, "\(counted.count) \(name(id)) signers for Android 15")
            }
        }
        for (digest, expected) in contentDigests.sorted(by: { $0.key < $1.key }) {
            guard try contentDigest(of: archive, blockOffset: blockOffset, digest) == expected else {
                throw SignatureFailure(.damaged, "the content does not match its \(name(id)) digest")
            }
        }
        if let unverifiable { return SignatureVerification(check: .unverified(unverifiable)) }
        let scheme: SignatureInfo.Scheme = switch id {
        case v2ID: .v2
        case v3ID: .v3
        default: .v31
        }
        return SignatureVerification(check: .verified, proof: SignerProof(
            scheme: scheme, certificates: counted.map(\.certificate), lineage: isV3 ? counted[0].lineage : []))
    }

    /// Whether a v3 or v3.1 signer is for Android 15, its Android versions read as the ints Android reads
    /// (ApkSignatureSchemeV3Verifier.java:274-292). A v3.1 signer for others leaves the lowest of their minSdk
    /// for v3's rotation-min-sdk attribute.
    static func isForAndroid15(_ signer: Signer, scheme id: UInt32, rotationMinSdk: inout Int32?) -> Bool {
        let minSdk = Int32(bitPattern: signer.minSdk)
        if minSdk <= platformSDK, platformSDK <= Int32(bitPattern: signer.maxSdk) { return true }
        if id == v31ID, rotationMinSdk.map({ $0 > minSdk }) ?? true { rotationMinSdk = minSdk }
        return false
    }

    /// A v3 or v3.1 signer's additional attributes in their order, as ApkSignatureSchemeV3Verifier's
    /// verifyAdditionalAttributes (444-527) reads them: the verified lineage, empty without one, at most one
    /// (459-461); v3.1's block for what the rotation-min-sdk attribute names (479-500); nil for a v3.1 signer for
    /// the development preview of Android 15, which a release ("REL") skips (502-519).
    static func verifyAttributes(of signer: Signer, scheme id: UInt32,
                                 rotationMinSdk: inout Int32?) throws -> [SignerProof.Level]? {
        var lineage: [SignerProof.Level]?
        for attribute in signer.attributes {
            switch attribute.id {
            case proofOfRotationID:
                guard lineage == nil else {
                    throw SignatureFailure(.signatureMismatch, "a \(name(id)) signer has two proofs of rotation")
                }
                lineage = try verifyLineage(attribute.value, of: signer)
            case rotationMinSdkID:
                // v3.1's block must be there and target what this attribute names; otherwise someone removed it.
                guard attribute.value.count >= 4 else { throw SignatureFailure(.damaged, "a short rotation-min-sdk attribute") }
                let named = Int32(bitPattern: try ByteReader(attribute.value).u32(at: 0))
                guard let rotationMinSdk else {
                    throw SignatureFailure(.signatureMismatch, "the v3.1 signature for SDK \(named) on was removed")
                }
                guard rotationMinSdk == named else {
                    throw SignatureFailure(.signatureMismatch,
                                           "the \(name(id)) signer names v3.1 for SDK \(named) on, but v3.1 targets \(rotationMinSdk)")
                }
            case rotationOnDevReleaseID where id == v31ID && Int32(bitPattern: signer.signedMinSdk) == platformSDK:
                rotationMinSdk = platformSDK
                return nil
            default:
                break
            }
        }
        return lineage ?? []
    }

    static func signer(_ data: Data, isV3: Bool) throws -> Signer {
        var result = Signer()
        var reader = ByteReader(data, context: "signer")
        result.signedData = try prefixed(&reader)
        if isV3 {
            result.minSdk = try reader.u32()
            result.maxSdk = try reader.u32()
        }
        result.signatures = try records(try prefixed(&reader), "signatures")
        result.publicKey = try prefixed(&reader)

        var signedData = ByteReader(result.signedData, context: "signed data")
        result.digests = try records(try prefixed(&signedData), "digests")
        result.certificates = try list(try prefixed(&signedData), "certificates")
        if isV3 {
            result.signedMinSdk = try signedData.u32()
            result.signedMaxSdk = try signedData.u32()
        }
        for value in try list(try prefixed(&signedData), "additional attributes") {
            var attribute = ByteReader(value, context: "attribute")
            result.attributes.append((try attribute.u32(), try attribute.bytes(attribute.remaining)))
        }
        return result
    }

    /// Checks one signer's signature with the strongest algorithm Security.framework can check, and
    /// returns the content digest it signed.
    static func verifySigner(_ signer: Signer, scheme id: UInt32, present: Set<UInt32>) throws -> (ContentDigest, Data) {
        let scheme = name(id)
        guard !signer.signatures.isEmpty else { throw SignatureFailure(.signatureMismatch, "a \(scheme) signer has no signature") }
        guard let certificate = signer.certificates.first else {
            throw SignatureFailure(.signatureMismatch, "a \(scheme) signer has no certificate")
        }
        guard signer.signatures.map(\.algorithm) == signer.digests.map(\.algorithm) else {
            throw SignatureFailure(.signatureMismatch, "the \(scheme) digests and signatures name other algorithms")
        }
        if id != v2ID, signer.signedMinSdk != signer.minSdk || signer.signedMaxSdk != signer.maxSdk {
            throw SignatureFailure(.signatureMismatch, "the \(scheme) signer's Android versions differ from its signed data's")
        }
        let known = signer.signatures.compactMap { signature in algorithm(signature.algorithm).map { (signature, $0) } }
        guard !known.isEmpty else { throw SignatureFailure(.signatureMismatch, "no \(scheme) signature algorithm Android knows") }
        let checkable = known.filter { $0.1.secAlgorithm != nil }
        guard let (signature, chosen) = checkable.max(by: { $0.1.digest! < $1.1.digest! }),
              let secAlgorithm = chosen.secAlgorithm, let digest = chosen.digest
        else {
            throw Unverifiable(reason: "signed with \(known.map(\.1.name).joined(separator: ", "))")
        }
        // As Android: the signer's key byte for byte with the certificate's, re-encoded (javaEncoding).
        let mismatch = SignatureFailure(.signatureMismatch, "the \(scheme) signer's public key is not its certificate's")
        let key: SecKey
        do {
            key = try publicKey(subjectPublicKeyInfo: signer.publicKey)
            guard try javaEncoding(ofKeyIn: certificate) == signer.publicKey else { throw mismatch }
        } catch let reason as Unverifiable {
            // A key Security.framework cannot read is the certificate's only with the same bytes, and
            // then proves nothing.
            guard (try? X509.subjectPublicKeyInfo(of: certificate)) == signer.publicKey else { throw mismatch }
            throw reason
        }
        guard verifies(key, secAlgorithm, signer.signedData, signature.value) else {
            throw SignatureFailure(.signatureMismatch, "the \(scheme) signature does not verify")
        }

        for attribute in signer.attributes where id == v2ID && attribute.id == strippingProtectionID {
            // ApkSignatureSchemeV2Verifier.java:384-388: a value under 4 bytes fails the signer.
            guard attribute.value.count >= 4 else {
                throw SignatureFailure(.damaged, "the v2 stripping protection is \(attribute.value.count) bytes")
            }
            if try ByteReader(attribute.value).u32(at: 0) == 3, !present.contains(v3ID) {
                throw SignatureFailure(.signatureMismatch, "the v3 signature was removed")
            }
        }
        guard let value = signer.digests.first(where: { $0.algorithm == signature.algorithm })?.value else {
            throw SignatureFailure(.signatureMismatch, "the \(scheme) signer has no digest for its signature")
        }
        return (digest, value)
    }

    /// A v3 signer's proof of rotation, oldest first, each level verified with the key of the level before
    /// and the algorithm that level names, as ApkSigningBlockUtils.verifyProofOfRotationStruct (819-906) reads
    /// it: its version is not looked at, and a certificate may appear once. A lineage ends with the signer's own
    /// certificate (ApkSignatureSchemeV3Verifier.java:462-473); one without levels is none.
    static func verifyLineage(_ value: Data, of signer: Signer) throws -> [SignerProof.Level] {
        var levels: [SignerProof.Level] = []
        var seen: Set<Data> = []
        do {
            var reader = ByteReader(value, context: "proof of rotation")
            _ = try reader.u32() // the version
            var previous: (certificate: Data, algorithm: UInt32)?
            while reader.remaining > 0 {
                guard levels.count < APKSignatures.maximumListLength else {
                    throw SignatureFailure(.damaged, "a lineage of more than \(APKSignatures.maximumListLength) levels")
                }
                var level = ByteReader(try prefixed(&reader), context: "rotation level")
                let signedData = try prefixed(&level)
                let flags = try level.u32()
                let algorithmID = try level.u32()
                let signature = try prefixed(&level)
                if let previous {
                    guard let algorithm = algorithm(previous.algorithm) else {
                        throw SignatureFailure(.signatureMismatch, "the lineage names an unknown algorithm")
                    }
                    guard let secAlgorithm = algorithm.secAlgorithm else {
                        throw Unverifiable(reason: "its lineage is signed with \(algorithm.name)")
                    }
                    guard verifies(try publicKey(of: previous.certificate), secAlgorithm, signedData, signature) else {
                        throw SignatureFailure(.signatureMismatch, "the lineage is broken at level \(levels.count + 1)")
                    }
                }
                var signed = ByteReader(signedData, context: "rotation signed data")
                let certificate = try prefixed(&signed)
                let signedAlgorithm = try signed.u32()
                if let previous, signedAlgorithm != previous.algorithm {
                    throw SignatureFailure(.signatureMismatch, "the lineage's algorithms differ at level \(levels.count + 1)")
                }
                guard seen.insert(certificate).inserted else {
                    throw SignatureFailure(.signatureMismatch, "a certificate appears twice in the lineage")
                }
                levels.append(SignerProof.Level(certificate: certificate, flags: flags))
                previous = (certificate, algorithmID)
            }
        } catch let error as SignatureFailure {
            throw error
        } catch let error as Unverifiable {
            throw error
        } catch {
            throw SignatureFailure(.damaged, "the proof of rotation: \(error)")
        }
        guard levels.isEmpty || levels.last?.certificate == signer.certificates.first else {
            throw SignatureFailure(.signatureMismatch, "the lineage does not end with the signer's certificate")
        }
        return levels
    }

    /// The content digest of v2 and v3: the entries up to the signing block, the central directory and
    /// the end record with the signing block's offset as the central directory's, in chunks of 1 MiB.
    static func contentDigest(of archive: ZipArchive, blockOffset: UInt64, _ digest: ContentDigest) throws -> Data {
        switch digest {
        case .sha256: try chunkedDigest(of: archive, blockOffset: blockOffset, SHA256.self)
        case .sha512: try chunkedDigest(of: archive, blockOffset: blockOffset, SHA512.self)
        }
    }

    static func chunkedDigest<H: HashFunction>(of archive: ZipArchive, blockOffset: UInt64, _: H.Type) throws -> Data {
        let source = archive.source
        let endRecord = archive.endOfCentralDirectoryOffset
        guard archive.centralDirectoryOffset + archive.centralDirectorySize == endRecord else {
            throw SignatureFailure(.damaged, "the central directory is not followed by its end record")
        }
        guard blockOffset <= UInt64(UInt32.max) else { throw SignatureFailure(.damaged, "a signing block beyond 4 GiB") }
        var end = try source.read(at: endRecord, count: Int(source.size - endRecord))
        guard end.count >= 22 else { throw SignatureFailure(.damaged, "a short end record") }
        withUnsafeBytes(of: UInt32(blockOffset).littleEndian) { end.replaceSubrange(16..<20, with: $0) }

        var chunkDigests = Data()
        var count: UInt32 = 0
        func add(_ chunk: Data) {
            var hasher = H()
            hasher.update(data: Data([0xA5]))
            withUnsafeBytes(of: UInt32(chunk.count).littleEndian) { hasher.update(bufferPointer: $0) }
            hasher.update(data: chunk)
            chunkDigests.append(contentsOf: hasher.finalize())
            count += 1
        }
        for (start, stop) in [(UInt64(0), blockOffset), (archive.centralDirectoryOffset, endRecord)] {
            var offset = start
            while offset < stop {
                let size = Int(min(UInt64(chunkSize), stop - offset))
                add(try source.read(at: offset, count: size))
                offset += UInt64(size)
            }
        }
        var offset = 0
        while offset < end.count {
            let size = min(chunkSize, end.count - offset)
            add(end.subdata(in: offset..<offset + size))
            offset += size
        }
        var hasher = H()
        hasher.update(data: Data([0x5A]))
        withUnsafeBytes(of: count.littleEndian) { hasher.update(bufferPointer: $0) }
        hasher.update(data: chunkDigests)
        return Data(hasher.finalize())
    }

    // MARK: - Parsing

    static func prefixed(_ reader: inout ByteReader) throws -> Data {
        try reader.bytes(Int(try reader.u32()))
    }

    /// The length-prefixed values in `data`, no more than `APKSignatures.maximumListLength`.
    static func list(_ data: Data, _ what: String) throws -> [Data] {
        var reader = ByteReader(data, context: what)
        var result: [Data] = []
        while reader.remaining > 0 {
            guard result.count < APKSignatures.maximumListLength else {
                throw APKError.invalidSigningBlock("more than \(APKSignatures.maximumListLength) \(what)")
            }
            result.append(try prefixed(&reader))
        }
        return result
    }

    /// Length-prefixed records of an algorithm ID and a length-prefixed value.
    static func records(_ data: Data, _ what: String) throws -> [(algorithm: UInt32, value: Data)] {
        try list(data, what).map { value in
            var record = ByteReader(value, context: what)
            return (try record.u32(), try prefixed(&record))
        }
    }
}

/// Signed with an algorithm Security.framework cannot check.
struct Unverifiable: Error {
    var reason: String
}
