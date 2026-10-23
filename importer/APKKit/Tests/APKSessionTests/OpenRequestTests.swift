import Darwin
import Foundation
import Testing

@testable import APKSession

/// ADR-0062: what the guest asks the host to open (APKRT_REQUEST_OPEN), checked again by the host's allow-list.
@Suite("OpenRequest")
struct OpenRequestTests {
    /// A field as the guest encodes it: every byte but RFC 3986's unreserved ones as %XX.
    static func field(_ text: String) -> String {
        text.utf8.map { byte in
            switch byte {
            case UInt8(ascii: "A")...UInt8(ascii: "Z"), UInt8(ascii: "a")...UInt8(ascii: "z"),
                 UInt8(ascii: "0")...UInt8(ascii: "9"), UInt8(ascii: "-"), UInt8(ascii: "."), UInt8(ascii: "_"),
                 UInt8(ascii: "~"):
                String(UnicodeScalar(byte))
            default:
                String(format: "%%%02X", byte)
            }
        }.joined()
    }

    static func text(_ kind: Int, _ fields: [String]) -> String {
        ([String(kind)] + fields.map(field)).joined(separator: "\t")
    }

    static func open(_ kind: Int, _ fields: String...) -> OpenRequest? {
        OpenRequest.parse(text(kind, fields))
    }

    @Test func httpsAndHttpGoToTheBrowserSheet() {
        #expect(Self.open(1, "https://example.org/page?a=1")?.action() == .browser(URL(string: "https://example.org/page?a=1")!))
        #expect(Self.open(1, "http://example.org")?.action() == .browser(URL(string: "http://example.org")!))
        // SFSafariViewController gets the scheme in lower case.
        #expect(Self.open(1, "HTTPS://example.org")?.action() == .browser(URL(string: "https://example.org")!))
        #expect(Self.open(1, "HtTp://Example.org/A?b=C")?.action() == .browser(URL(string: "http://Example.org/A?b=C")!))
        #expect(Self.open(1, "http:foo") == nil)
        #expect(Self.open(1, "HTTP:foo") == nil)
    }

    @Test func telAndMapsGoToIOS() {
        #expect(Self.open(1, "tel:+49301234567")?.action() == .systemOpen(URL(string: "tel:+49301234567")!))
        #expect(Self.open(1, "maps://?q=Berlin")?.action() == .systemOpen(URL(string: "maps://?q=Berlin")!))
    }

    @Test func schemesOutsideTheAllowListAreRefused() {
        for url in ["file:///etc/passwd", "javascript:alert(1)", "data:text/html,hi", "intent://x#Intent;end",
                    "market://details?id=a.b", "ftp://example.org", "minato://open", "sms:123", "mailto:a@b.org"] {
            #expect(Self.open(1, url) == nil, "\(url)")
        }
    }

    @Test func aBrowserURLHasNoUserInfo() {
        #expect(Self.open(1, "http://good.example:@evil.example") == nil)
        #expect(Self.open(1, "https://good.example%2F@evil.example") == nil)
        #expect(Self.open(1, "https://user:pass@example.org") == nil)
    }

    @Test func aBrowserURLNeedsAHost() {
        #expect(Self.open(1, "https:example.org") == nil)
        #expect(Self.open(1, "https://") == nil)
        #expect(Self.open(1, "https:///path") == nil)
        #expect(Self.open(1, "http://:80/") == nil)
        #expect(Self.open(1, "tel:") == nil)
    }

    @Test func aBadTextIsRefused() {
        #expect(OpenRequest.parse("") == nil)
        #expect(OpenRequest.parse("1") == nil)
        #expect(OpenRequest.parse("5\thttps%3A%2F%2Fexample.org") == nil)
        #expect(OpenRequest.parse("01\thttps%3A%2F%2Fexample.org") == nil)
        #expect(OpenRequest.parse("1\thttps%3A%2F%2Fexample.org\textra") == nil)
        #expect(OpenRequest.parse("1\thttps%3A%2F%2Fexample.org%G1") == nil)
        #expect(OpenRequest.parse("1\thttps%3A%2F%2Fexample.org%2") == nil)
        #expect(OpenRequest.parse("1\thttps://example.org") == nil, "raw reserved bytes")
        #expect(OpenRequest.parse("1\thttps%3A%2F%2Fexample.org%2F%FF") == nil, "no UTF-8")
        #expect(Self.open(1, "https://example.org/\n") == nil, "a control character")
        #expect(Self.open(1, "https://example.org/" + String(repeating: "a", count: 4096)) == nil)
        #expect(OpenRequest.parse("1\t%68ttps%3A%2F%2Fexample.org") == nil, "an unreserved byte encoded")
        #expect(OpenRequest.parse("%31\thttps%3A%2F%2Fexample.org") == nil)
        #expect(OpenRequest.parse("1\thttps%3A%2F%2Fexample.org%2F%00") == nil, "NUL")
    }

    @Test func aURLHasNoSpaceOrInvisibleCharacter() {
        for bad in [" ", "\u{A0}", "\u{2009}", "\u{3000}", "\u{AD}", "\u{61C}", "\u{202E}", "\u{E0001}", "\u{E000}"] {
            #expect(Self.open(1, "https://bank.example\(bad).evil.org") == nil, "\(bad.unicodeScalars.first!.value)")
        }
        #expect(Self.open(1, "https://example.org/\u{E4}") != nil)
    }

    @Test func theOutcomeIsTheEventsResultAndReason() {
        #expect(OpenOutcome.shown.resultCode == 0xFFFF_FFFF && OpenOutcome.shown.reason == 0)
        #expect(OpenOutcome.cancelled.resultCode == 0 && OpenOutcome.cancelled.reason == 0)
        #expect(OpenOutcome.refused.resultCode == 0 && OpenOutcome.refused.reason == 1)
        #expect(OpenOutcome.background.resultCode == 0 && OpenOutcome.background.reason == 2)
        #expect(OpenOutcome.noHandler.resultCode == 0 && OpenOutcome.noHandler.reason == 3)
    }

    @Test func aMailtoURLBecomesADraft() throws {
        let request = try #require(Self.open(2, "mailto:a@example.org,b@example.org?cc=c@example.org&bcc=d@example.org"
            + "&subject=Hi%20there&body=Line%201%0D%0ALine%202"))
        let draft = MailDraft(to: ["a@example.org", "b@example.org"], cc: ["c@example.org"], bcc: ["d@example.org"],
                              subject: "Hi there", body: "Line 1\r\nLine 2")
        #expect(request.action(canSendMail: true) == .mailComposer(draft))
        let url = URL(string: "mailto:a@example.org,b@example.org?cc=c@example.org&bcc=d@example.org"
            + "&subject=Hi%20there&body=Line%201%0D%0ALine%202")!
        #expect(request.action(canSendMail: false) == .systemOpen(url))
    }

    @Test func theMailAppGetsOnlyTheDraftsFields() throws {
        let request = try #require(Self.open(2, "mailto:a@example.org?to=b@example.org&x-attach=/etc/passwd&body=hi"))
        #expect(request.action(canSendMail: true)
            == .mailComposer(MailDraft(to: ["a@example.org", "b@example.org"], cc: [], bcc: [], subject: "", body: "hi")))
        #expect(request.action(canSendMail: false)
            == .systemOpen(URL(string: "mailto:a@example.org,b@example.org?body=hi")!))
    }

    @Test func aMailtoWithoutAddressKeepsItsSubject() throws {
        let request = try #require(Self.open(2, "mailto:?subject=Feedback"))
        #expect(request.action(canSendMail: true)
            == .mailComposer(MailDraft(to: [], cc: [], bcc: [], subject: "Feedback", body: "")))
    }

    @Test func mailAndSmsTakeOnlyTheirScheme() {
        #expect(Self.open(2, "https://example.org") == nil)
        #expect(Self.open(2, "sms:123") == nil)
        #expect(Self.open(3, "mailto:a@example.org") == nil)
        #expect(Self.open(3, "tel:123") == nil)
    }

    @Test func anSmsURLBecomesAMessage() throws {
        let request = try #require(Self.open(3, "sms:+491701234567&body=See%20you"))
        #expect(request.action(canSendText: true)
            == .messageComposer(MessageDraft(recipients: ["+491701234567"], body: "See you")))
        #expect(request.action(canSendText: false) == .systemOpen(URL(string: "sms:+491701234567&body=See%20you")!))
        #expect(Self.open(3, "sms:+4930,+4940?body=Hi")?.action(canSendText: true)
            == .messageComposer(MessageDraft(recipients: ["+4930", "+4940"], body: "Hi")))
        #expect(Self.open(3, "sms:+4930?x=1&body=Hi%26ho")?.action(canSendText: true)
            == .messageComposer(MessageDraft(recipients: ["+4930"], body: "Hi&ho")))
    }

    @Test func aShareCarriesItsTextAndFiles() throws {
        let request = try #require(Self.open(4, "Subject", "Text\twith a tab", "2", "a.png", "b c.txt", "image/png",
                                             "text/plain"))
        #expect(request.action() == .shareSheet(ShareRequest(subject: "Subject", text: "Text\twith a tab", files: [
            ShareFile(name: "a.png", type: "image/png"), ShareFile(name: "b c.txt", type: "text/plain"),
        ])))
        #expect(Self.open(4, "", "Only text", "0")?.action()
            == .shareSheet(ShareRequest(subject: "", text: "Only text", files: [])))
        #expect(Self.open(4, "", "", "1", "photo 1.jpg", "image/*")?.action()
            == .shareSheet(ShareRequest(subject: "", text: "", files: [ShareFile(name: "photo 1.jpg", type: "image/*")])))
    }

    @Test func aBadShareIsRefused() {
        #expect(Self.open(4, "", "", "1") == nil, "a name too few")
        #expect(Self.open(4, "", "", "1", "a.png") == nil, "no MIME type")
        #expect(Self.open(4, "", "", "01", "a.png", "image/png") == nil)
        #expect(Self.open(4, "", "", "11") == nil)
        for name in ["", ".", "..", "a/b", "a\u{0}b", "a\nb", "a\u{202E}b", "a\u{200B}b", "a\u{A0}b", "a\u{3000}b",
                     "a\u{61C}b"] {
            #expect(Self.open(4, "", "", "1", name, "image/png") == nil, "\(name.debugDescription)")
        }
        #expect(Self.open(4, "", "", "1", String(repeating: "a", count: 256), "image/png") == nil)
        #expect(Self.open(4, "", "", "2", "a.png", "a.png", "image/png", "image/png") == nil, "a name twice")
        #expect(Self.open(4, "", "", "3", "a.png", "b.png", "a.png", "image/png", "image/png", "image/png") == nil)
        #expect(Self.open(4, "", "", "1", "a.png", "image/png", "x") == nil, "a field too many")
        for type in ["", "image", "image/", "/png", "image/png/x", "image/p ng", String(repeating: "a", count: 99) + "/b"] {
            #expect(Self.open(4, "", "", "1", "a.png", type) == nil, "\(type)")
        }
        #expect(Self.open(4, String(repeating: "s", count: 1001), "", "0") == nil)
        #expect(Self.open(4, "", "a\u{0}b", "0") == nil, "NUL in the text")
        #expect(Self.open(4, "a\u{0}b", "", "0") == nil, "NUL in the subject")
        #expect(Self.open(4, "", String(repeating: "t", count: 65537), "0") == nil)
        #expect(Self.open(4, "", String(repeating: "t", count: 65536), "0") != nil)
    }

    @Test func aWholeTextOverTheLimitIsRefused() {
        // Each field within its limit, the whole over it: '!' is 3 bytes encoded.
        let names = (0..<10).map { String(repeating: "!", count: 254) + String($0) }
        let text = Self.text(4, [String(repeating: "!", count: 1000), String(repeating: "!", count: 65536), "10"]
            + names + Array(repeating: "a/a", count: 10))
        #expect(text.utf8.count > 204_800)
        #expect(OpenRequest.parse(text) == nil)
    }
}
