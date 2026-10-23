import Foundation

/// A mail the guest asks to write (ADR-0062, decision 2), from its mailto: URL (RFC 6068).
public struct MailDraft: Sendable, Equatable {
    public var to: [String]
    public var cc: [String]
    public var bcc: [String]
    public var subject: String
    public var body: String

    public init(to: [String], cc: [String], bcc: [String], subject: String, body: String) {
        self.to = to
        self.cc = cc
        self.bcc = bcc
        self.subject = subject
        self.body = body
    }

    /// The draft as a mailto: URL of its fields alone, for the Mail app when the device cannot send mail here.
    public var url: URL? {
        var components = URLComponents()
        components.scheme = "mailto"
        components.path = to.joined(separator: ",")
        let items = [("cc", cc.joined(separator: ",")), ("bcc", bcc.joined(separator: ",")), ("subject", subject),
                     ("body", body)].filter { !$0.1.isEmpty }.map { URLQueryItem(name: $0.0, value: $0.1) }
        components.queryItems = items.isEmpty ? nil : items
        return components.url
    }
}

/// A text message the guest asks to write (ADR-0062, decision 2), from its sms: URL.
public struct MessageDraft: Sendable, Equatable {
    public var recipients: [String]
    public var body: String

    public init(recipients: [String], body: String) {
        self.recipients = recipients
        self.body = body
    }
}

/// One file of a share: its name in the share folder and its MIME type.
public struct ShareFile: Sendable, Equatable {
    public var name: String
    public var type: String

    public init(name: String, type: String) {
        self.name = name
        self.type = type
    }
}

/// What the guest asks to share (ADR-0062, decision 3): ACTION_SEND's subject, text and streams, the streams copied by
/// the guest into its share folder.
public struct ShareRequest: Sendable, Equatable {
    public var subject: String
    public var text: String
    public var files: [ShareFile]

    public init(subject: String, text: String, files: [ShareFile]) {
        self.subject = subject
        self.text = text
        self.files = files
    }
}

/// How the host shows a request.
public enum OpenAction: Sendable, Equatable {
    /// SFSafariViewController inside Minato: http and https only.
    case browser(URL)
    /// UIApplication.open, which asks the user where iOS asks (tel).
    case systemOpen(URL)
    /// MFMailComposeViewController.
    case mailComposer(MailDraft)
    /// MFMessageComposeViewController.
    case messageComposer(MessageDraft)
    /// UIActivityViewController.
    case shareSheet(ShareRequest)
}

/// How a request ended, as APKRT_EVENT_OPEN_RESULT tells the guest.
public enum OpenOutcome: Sendable, Equatable {
    /// The sheet, the composer or iOS's open reported that it was shown or done.
    case shown
    /// The user cancelled.
    case cancelled
    /// The allow-list, URL(string:) or a share file refused it.
    case refused
    /// The session was in the background.
    case background
    /// Nothing on this device can show it.
    case noHandler

    /// The event's a1, the result code as a 32-bit int in the low bits (RESULT_OK -1, RESULT_CANCELED 0).
    public var resultCode: UInt64 { self == .shown ? 0xFFFF_FFFF : 0 }

    /// The event's a2, why.
    public var reason: UInt64 {
        switch self {
        case .shown, .cancelled: 0
        case .refused: 1
        case .background: 2
        case .noHandler: 3
        }
    }
}

/// What the guest asks the host to open (APKRT_REQUEST_OPEN, ADR-0062), as the host parses it again: the runtime has
/// checked the text against guest_interface.h's grammar, and the host is the last word. It opens only http, https,
/// tel and maps URLs, mailto: and sms: drafts and shares.
public enum OpenRequest: Sendable, Equatable {
    case view(URL)
    case mail(MailDraft, URL)
    case message(MessageDraft, URL)
    case share(ShareRequest)

    static let textLimit = 204_800
    static let urlLimit = 4096
    static let subjectLimit = 1000
    static let shareTextLimit = 65536
    static let nameLimit = 255
    static let filesLimit = 10

    /// The request of the runtime's text, or nil when the host refuses it.
    public static func parse(_ text: String) -> OpenRequest? {
        guard text.utf8.count <= textLimit else { return nil }
        let encoded = text.split(separator: "\t", omittingEmptySubsequences: false)
        guard let kind = encoded.first, kind.count == 1, let number = Int(kind), (1...4).contains(number) else {
            return nil
        }
        var fields: [String] = []
        for field in encoded.dropFirst() {
            guard let decoded = decode(field) else { return nil }
            fields.append(decoded)
        }
        switch number {
        case 1:
            guard fields.count == 1, let url = url(fields[0], schemes: ["http", "https", "tel", "maps"]) else {
                return nil
            }
            return .view(url)
        case 2:
            guard fields.count == 1, let url = url(fields[0], schemes: ["mailto"]) else { return nil }
            return mail(url).map { .mail($0, url) }
        case 3:
            guard fields.count == 1, let url = url(fields[0], schemes: ["sms"]) else { return nil }
            return message(url).map { .message($0, url) }
        default:
            return share(fields)
        }
    }

    /// How the host shows it: a mail or a message in its composer when the device can send one, else by iOS's open.
    public func action(canSendMail: Bool = false, canSendText: Bool = false) -> OpenAction {
        switch self {
        case .view(let url):
            let scheme = url.scheme?.lowercased()
            return scheme == "http" || scheme == "https" ? .browser(url) : .systemOpen(url)
        case .mail(let draft, let url):
            return canSendMail ? .mailComposer(draft) : .systemOpen(draft.url ?? url)
        case .message(let draft, let url):
            return canSendText ? .messageComposer(draft) : .systemOpen(url)
        case .share(let request):
            return .shareSheet(request)
        }
    }

    /// A field decoded: RFC 3986's unreserved bytes raw and never encoded, every other byte as % and two hex digits,
    /// the bytes UTF-8 without NUL.
    static func decode(_ field: Substring) -> String? {
        var bytes: [UInt8] = []
        var iterator = field.utf8.makeIterator()
        while let byte = iterator.next() {
            if unreserved(byte) {
                bytes.append(byte)
                continue
            }
            guard byte == UInt8(ascii: "%"), let high = iterator.next().flatMap(hex), let low = iterator.next().flatMap(hex)
            else { return nil }
            let decoded = high << 4 | low
            guard !unreserved(decoded), decoded != 0 else { return nil }
            bytes.append(decoded)
        }
        return String(validating: bytes, as: UTF8.self)
    }

    static func unreserved(_ byte: UInt8) -> Bool {
        switch byte {
        case UInt8(ascii: "A")...UInt8(ascii: "Z"), UInt8(ascii: "a")...UInt8(ascii: "z"),
             UInt8(ascii: "0")...UInt8(ascii: "9"), UInt8(ascii: "-"), UInt8(ascii: "."), UInt8(ascii: "_"),
             UInt8(ascii: "~"):
            true
        default:
            false
        }
    }

    static func hex(_ byte: UInt8) -> UInt8? {
        switch byte {
        case UInt8(ascii: "0")...UInt8(ascii: "9"): byte - UInt8(ascii: "0")
        case UInt8(ascii: "a")...UInt8(ascii: "f"): byte - UInt8(ascii: "a") + 10
        case UInt8(ascii: "A")...UInt8(ascii: "F"): byte - UInt8(ascii: "A") + 10
        default: nil
        }
    }

    /// Whether a code point shows: no control, format, private-use or unassigned one, no space (but U+0020 when
    /// `space` allows it), no line or paragraph separator.
    static func visible(_ scalar: Unicode.Scalar, space: Bool) -> Bool {
        if scalar == " " { return space }
        switch scalar.properties.generalCategory {
        case .control, .format, .spaceSeparator, .lineSeparator, .paragraphSeparator, .privateUse, .surrogate,
             .unassigned:
            return false
        default:
            return true
        }
    }

    /// The URL of a field whose scheme is one of `schemes` (lower case), of visible code points without spaces; an
    /// http or https URL needs "//" and a host, has no user info (which would show a host other than the one it opens)
    /// and gets its scheme in lower case, as SFSafariViewController takes only such URLs; any other needs something
    /// after its scheme.
    static func url(_ field: String, schemes: Set<String>) -> URL? {
        guard !field.isEmpty, field.utf8.count <= urlLimit, field.unicodeScalars.allSatisfy({ visible($0, space: false) }),
              let url = URL(string: field), let scheme = url.scheme?.lowercased(), schemes.contains(scheme)
        else { return nil }
        if scheme == "http" || scheme == "https" {
            guard field.lowercased().hasPrefix(scheme + "://"),
                  var components = URLComponents(url: url, resolvingAgainstBaseURL: false),
                  let host = components.host, !host.isEmpty, components.user == nil, components.password == nil
            else { return nil }
            components.scheme = scheme
            guard let normalized = components.url, normalized.scheme == scheme, normalized.host() != nil else {
                return nil
            }
            return normalized
        }
        return field.utf8.count > scheme.utf8.count + 1 ? url : nil
    }

    /// Addresses or numbers, comma-separated and percent-encoded, without the empty ones.
    static func list(_ text: Substring) -> [String] {
        text.split(separator: ",").compactMap { $0.removingPercentEncoding?.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
    }

    /// RFC 6068's draft: to in the path, to, cc, bcc, subject and body in the query; other headers are ignored.
    static func mail(_ url: URL) -> MailDraft? {
        guard let components = URLComponents(url: url, resolvingAgainstBaseURL: false) else { return nil }
        var draft = MailDraft(to: list(Substring(components.percentEncodedPath)), cc: [], bcc: [], subject: "", body: "")
        for item in components.percentEncodedQueryItems ?? [] {
            let value = item.value ?? ""
            switch item.name.lowercased() {
            case "to": draft.to += list(Substring(value))
            case "cc": draft.cc += list(Substring(value))
            case "bcc": draft.bcc += list(Substring(value))
            case "subject": draft.subject = value.removingPercentEncoding ?? ""
            case "body": draft.body = value.removingPercentEncoding ?? ""
            default: break
            }
        }
        return draft
    }

    /// "sms:<numbers>" with "&body=" (iOS's form) or "?body=" (RFC 5724's) after it.
    static func message(_ url: URL) -> MessageDraft? {
        let text = url.absoluteString.dropFirst(4)  // "sms:"
        let end = text.firstIndex { $0 == "?" || $0 == "&" } ?? text.endIndex
        var draft = MessageDraft(recipients: list(text[..<end]), body: "")
        if end < text.endIndex {
            for parameter in text[text.index(after: end)...].split(separator: "&") {
                let pair = parameter.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
                if pair.count == 2, pair[0].lowercased() == "body" {
                    draft.body = String(pair[1]).removingPercentEncoding ?? ""
                }
            }
        }
        return draft
    }

    /// A share's subject, text, the count n and n names and n MIME types.
    static func share(_ fields: [String]) -> OpenRequest? {
        guard fields.count >= 3, fields[0].utf8.count <= subjectLimit, fields[1].utf8.count <= shareTextLimit,
              let count = Int(fields[2]), String(count) == fields[2], (0...filesLimit).contains(count),
              fields.count == 3 + 2 * count
        else { return nil }
        let names = Array(fields[3..<3 + count]), types = Array(fields[3 + count..<3 + 2 * count])
        guard names.allSatisfy(name), Set(names).count == count, types.allSatisfy(mimeType) else { return nil }
        return .share(ShareRequest(subject: fields[0], text: fields[1],
                                   files: zip(names, types).map { ShareFile(name: $0, type: $1) }))
    }

    /// A file name of the share folder: 1 to 255 bytes of visible code points or spaces, without "/", not "." or "..".
    static func name(_ name: String) -> Bool {
        !name.isEmpty && name.utf8.count <= nameLimit && name != "." && name != ".." && !name.contains("/")
            && name.unicodeScalars.allSatisfy { visible($0, space: true) }
    }

    /// "<type>/<subtype>" of RFC 6838's characters or "*", 3 to 100 bytes.
    static func mimeType(_ type: String) -> Bool {
        let parts = type.split(separator: "/", omittingEmptySubsequences: false)
        let token = { (part: Substring) in
            !part.isEmpty && part.utf8.allSatisfy { byte in
                (byte < 0x80 && (Character(UnicodeScalar(byte)).isLetter || Character(UnicodeScalar(byte)).isNumber))
                    || "!#$&^_.+-*".utf8.contains(byte)
            }
        }
        return (3...100).contains(type.utf8.count) && parts.count == 2 && parts.allSatisfy(token)
    }
}
