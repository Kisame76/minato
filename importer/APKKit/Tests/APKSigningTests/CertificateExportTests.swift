import Foundation
import Testing

@testable import APKSigning

@Suite("Certificate export and callback")
struct CertificateExportTests {
    static let now = Date(timeIntervalSince1970: 1_790_000_000)
    /// Bytes whose base64 contains "+", "/" and "=".
    static let p12 = Data((0..<3001).map { UInt8(truncatingIfNeeded: $0 &* 251 &+ 7) })
    static let token = String(repeating: "0", count: 32)

    /// Does what SideStore and AltStore do with an export link (docs/research/store-certificates.md,
    /// section 2), independently of the code under test: take `callback_template`, decode it twice,
    /// put in the base64 of `p12` percent-encoded with urlQueryAllowed minus ";/?:@&=+$, " and the
    /// password raw, then open the result with URL(string:).
    func storeAnswer(to request: URL, p12: Data, password: String) -> URL? {
        let items = URLComponents(url: request, resolvingAgainstBaseURL: false)?.queryItems ?? []
        guard let once = items.first(where: { $0.name.lowercased() == "callback_template" })?.value,
              let template = once.removingPercentEncoding
        else { return nil }
        var allowed = CharacterSet.urlQueryAllowed
        allowed.remove(charactersIn: ";/?:@&=+$, ")
        let certificate = p12.base64EncodedString().addingPercentEncoding(withAllowedCharacters: allowed)!
        return URL(string: template.replacingOccurrences(of: "$(BASE64_CERT)", with: certificate)
            .replacingOccurrences(of: "$(PASSWORD)", with: password))
    }

    @Test func requestURLCarriesTheTemplateEncodedOnce() throws {
        let request = ExportRequest(store: .altStore, token: String(repeating: "ab", count: 16), created: Self.now)
        let components = try #require(URLComponents(url: request.url, resolvingAgainstBaseURL: false))
        #expect(components.scheme == "altstore-classic")
        #expect(components.host == "certificate")
        #expect(components.queryItems?.count == 1)
        let template = try #require(components.queryItems?.first { $0.name == "callback_template" }?.value)
        #expect(template == "apkrunner://certificate?request=\(request.token)&cert=$(BASE64_CERT)&password=$(PASSWORD)")
        #expect(template.removingPercentEncoding == template)  // survives the stores' second decode
    }

    @Test func sideStoreRequest() {
        #expect(ExportRequest(store: .sideStore, created: Self.now).url.absoluteString.hasPrefix("sidestore://certificate?callback_template="))
    }

    @Test func tokensAreFreshLowercaseHex() {
        let token = ExportRequest.newToken()
        #expect(token.count == 32)
        #expect(token.allSatisfy { $0.isHexDigit && !$0.isUppercase })
        #expect(token != ExportRequest.newToken())
    }

    @Test(arguments: ["", "abc123", "0A1B2C3D", "a&b", "a#b", "a+b", "a%41b", "x y", "ä=ö"])
    func roundTripThroughAStore(password: String) throws {
        let request = ExportRequest(store: .sideStore, created: Self.now)
        let url = try #require(storeAnswer(to: request.url, p12: Self.p12, password: password))
        let callback = try CertificateCallback.parse(url)
        #expect(callback.token == request.token)
        #expect(callback.certificate == Self.p12)
        #expect(callback.passwordCandidates.contains(password))
        try callback.check(against: request, now: Self.now.addingTimeInterval(599))
    }

    @Test func rawPasswordComesFirst() throws {
        let url = try #require(URL(string: "apkrunner://certificate?request=\(Self.token)&cert=AA%3D%3D&password=a%41b"))
        #expect(try CertificateCallback.parse(url).passwordCandidates == ["a%41b", "aAb"])
        let plain = try #require(URL(string: "apkrunner://certificate?request=\(Self.token)&cert=AA%3D%3D&password=abc"))
        #expect(try CertificateCallback.parse(plain).passwordCandidates == ["abc"])
    }

    @Test func toleratesSpacesAndTheURLSafeAlphabet() throws {
        let base64 = Self.p12.base64EncodedString()
        let spaced = base64.replacingOccurrences(of: "+", with: "%20")
        let urlSafe = base64.replacingOccurrences(of: "+", with: "-").replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
        let doubleEncoded = base64.replacingOccurrences(of: "+", with: "%252B")
        for cert in [spaced, urlSafe, doubleEncoded] {
            let url = try #require(URL(string: "apkrunner://certificate?request=\(Self.token)&cert=\(cert)&password="))
            #expect(try CertificateCallback.parse(url).certificate == Self.p12)
        }
    }

    @Test(arguments: [
        ("https://certificate?request=\(token)&cert=AA%3D%3D&password=", CallbackError.notACertificateCallback),
        ("apkrunner://other?request=\(token)&cert=AA%3D%3D&password=", .notACertificateCallback),
        ("apkrunner://certificate?cert=AA%3D%3D&password=", .missingField("request")),
        ("apkrunner://certificate?request=\(token)&password=", .missingField("cert")),
        ("apkrunner://certificate?request=\(token)&cert=&password=", .missingField("cert")),
        ("apkrunner://certificate?request=\(token)&cert=AA%3D%3D", .missingField("password")),
        ("apkrunner://certificate?request=\(token)&cert=%21%21%21&password=", .badCertificate),
    ])
    func rejectsWhatIsNotACallback(text: String, error: CallbackError) throws {
        let url = try #require(URL(string: text))
        #expect(throws: error) { try CertificateCallback.parse(url) }
    }

    @Test func rejectsOversizedCertificates() throws {
        let big = Data(count: CertificateCallback.maximumCertificateSize + 1).base64EncodedString()
        let url = try #require(URL(string: "apkrunner://certificate?request=\(Self.token)&cert=\(big)&password="))
        #expect(throws: CallbackError.tooLarge) { try CertificateCallback.parse(url) }
    }

    @Test func checksTheRequest() throws {
        let request = ExportRequest(store: .altStore, created: Self.now)
        let callback = try CertificateCallback.parse(try #require(storeAnswer(to: request.url, p12: Self.p12, password: "")))
        #expect(throws: CallbackError.noPendingRequest) { try callback.check(against: nil, now: Self.now) }
        #expect(throws: CallbackError.wrongRequest) {
            try callback.check(against: ExportRequest(store: .altStore, created: Self.now), now: Self.now)
        }
        #expect(throws: CallbackError.expired) { try callback.check(against: request, now: Self.now.addingTimeInterval(601)) }
        try callback.check(against: request, now: Self.now.addingTimeInterval(600))
    }
}
