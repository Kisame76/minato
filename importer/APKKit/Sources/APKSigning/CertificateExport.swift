import Foundation

/// Asks a store for the certificate through its `certificate` export link (ADR-0023). The store
/// shows a dialog, the user taps Export, and the store opens `callbackTemplate` with the
/// certificate filled in. Neither store answers on Cancel or on an error, so a request that never
/// comes back is normal (docs/research/store-certificates.md, section 2).
public struct ExportRequest: Sendable, Equatable, Codable {
    /// How long a callback for this request is accepted.
    public static let lifetime: TimeInterval = 600

    public let store: Store
    /// Ties the callback to this request, so that no other app can plant a certificate unasked.
    public let token: String
    public let created: Date

    public init(store: Store, token: String = ExportRequest.newToken(), created: Date) {
        self.store = store
        self.token = token
        self.created = created
    }

    /// 32 lowercase hex digits.
    public static func newToken() -> String {
        UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "")
    }

    /// Where the store answers. The password comes last because both stores put it in raw, so it
    /// may contain "&" or "#"; the certificate comes percent-encoded.
    public var callbackTemplate: String {
        "\(CertificateCallback.scheme)://\(CertificateCallback.host)?request=\(token)&cert=$(BASE64_CERT)&password=$(PASSWORD)"
    }

    /// The link that opens the store. Both stores decode `callback_template` twice, so it is encoded
    /// exactly once and contains no "%" of its own.
    public var url: URL {
        let unreserved = CharacterSet(charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")
        let template = callbackTemplate.addingPercentEncoding(withAllowedCharacters: unreserved) ?? ""
        return URL(string: "\(store.exportScheme)://certificate?callback_template=\(template)")!
    }
}

public enum CallbackError: Error, Equatable, Sendable {
    case notACertificateCallback
    case missingField(String)
    case badCertificate
    case tooLarge
    case noPendingRequest
    case wrongRequest
    case expired
}

/// A store's answer to an `ExportRequest`: `minato://certificate?request=…&cert=…&password=…`.
public struct CertificateCallback: Sendable, Equatable {
    public static let scheme = "minato"
    public static let host = "certificate"
    /// A .p12 with one certificate and its key is a few KiB.
    public static let maximumCertificateSize = 65_536

    public let token: String
    /// The .p12 file.
    public let certificate: Data
    /// The password as it arrived, then percent-decoded if that differs. The stores put it in raw,
    /// so either may be the one the user typed.
    public let passwordCandidates: [String]

    public static func parse(_ url: URL) throws(CallbackError) -> CertificateCallback {
        guard url.scheme?.lowercased() == scheme, url.host(percentEncoded: true)?.lowercased() == host else {
            throw .notACertificateCallback
        }
        let query = url.query(percentEncoded: true) ?? ""
        // The password runs to the end, and a raw "#" in it starts the fragment.
        var head = Substring(query)
        var password: String?
        if let range = query.range(of: "&password=") {
            head = query[..<range.lowerBound]
            password = String(query[range.upperBound...])
        } else if query.hasPrefix("password=") {
            head = ""
            password = String(query.dropFirst("password=".count))
        }
        if password != nil, let fragment = url.fragment(percentEncoded: true) {
            password! += "#" + fragment
        }

        var fields: [String: String] = [:]
        for pair in head.split(separator: "&") {
            let parts = pair.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
            let name = parts[0].lowercased()
            if fields[name] == nil { fields[name] = parts.count > 1 ? String(parts[1]) : "" }
        }
        guard let token = fields["request"], !token.isEmpty else { throw .missingField("request") }
        guard let text = fields["cert"], !text.isEmpty else { throw .missingField("cert") }
        guard let password else { throw .missingField("password") }

        var candidates = [password]
        if let decoded = password.removingPercentEncoding, decoded != password {
            candidates.append(decoded)
        }
        return CertificateCallback(token: token, certificate: try decodeCertificate(text), passwordCandidates: candidates)
    }

    /// Accepts the callback only for the pending request, within its lifetime.
    public func check(against request: ExportRequest?, now: Date) throws(CallbackError) {
        guard let request else { throw .noPendingRequest }
        guard request.token == token else { throw .wrongRequest }
        guard now <= request.created.addingTimeInterval(ExportRequest.lifetime) else { throw .expired }
    }

    /// Standard base64 as the stores send it, percent-encoded once. Also takes it encoded twice, with
    /// spaces for "+", or in the URL-safe alphabet without padding, as other senders might.
    private static func decodeCertificate(_ text: String) throws(CallbackError) -> Data {
        guard var base64 = text.removingPercentEncoding else { throw .badCertificate }
        if base64.contains("%") {
            guard let again = base64.removingPercentEncoding else { throw .badCertificate }
            base64 = again
        }
        base64 = base64.replacingOccurrences(of: " ", with: "+")
            .replacingOccurrences(of: "-", with: "+")
            .replacingOccurrences(of: "_", with: "/")
        if base64.count % 4 != 0 {
            base64 += String(repeating: "=", count: 4 - base64.count % 4)
        }
        guard base64.count <= maximumCertificateSize * 4 / 3 + 4 else { throw .tooLarge }
        guard let data = Data(base64Encoded: base64), !data.isEmpty else { throw .badCertificate }
        guard data.count <= maximumCertificateSize else { throw .tooLarge }
        return data
    }
}
