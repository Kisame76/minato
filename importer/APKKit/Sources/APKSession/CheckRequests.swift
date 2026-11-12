import Foundation

/// The Debug check's requests without the phone's file service. tools/java-start-check.sh leaves its requests as files
/// in tmp/session-check/; on the iPhone each copy there is a session with the phone's file service, which can hang.
/// Two ways in that need none:
/// - at launch, the whole run's requests in MINATO_CHECK_REQUESTS, one "name=value" line each ("name" alone means
///   "yes"; a value's newlines as %0A, its percent signs as %25), through `devicectl device process launch
///   --environment-variables`;
/// - in the middle of a session, minato://check/<name>?<value> (no value: "yes"), through `devicectl device process
///   launch --payload-url` or `simctl openurl`.
/// Both become the files the script's copy would have left there, so the app reads them as it always did. A name is a
/// plain file name of lower-case letters, digits and dashes; anything else is dropped.
public enum CheckRequests {
    public static let variable = "MINATO_CHECK_REQUESTS"

    public struct Request: Equatable, Sendable {
        public let name: String
        public let value: String

        public init(name: String, value: String) {
            self.name = name
            self.value = value
        }
    }

    /// The launch's requests, in their order; none without the variable.
    public static func launch(environment: [String: String]) -> [Request] {
        guard let text = environment[variable] else { return [] }
        return text.split(separator: "\n", omittingEmptySubsequences: true).compactMap { line in
            let parts = line.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
            let name = String(parts[0])
            guard isName(name) else { return nil }
            let value = parts.count > 1 ? String(parts[1]) : "yes"
            return Request(name: name, value: value.removingPercentEncoding ?? value)
        }
    }

    /// Whether `url` is meant for the check (minato://check/...), valid or not: such a URL is never a file to open.
    public static func isCheck(_ url: URL) -> Bool {
        url.scheme == "minato" && url.host() == "check"
    }

    /// The request of minato://check/<name>?<value>; nil for any other URL or a name that is no plain file name.
    public static func request(from url: URL) -> Request? {
        guard isCheck(url), let components = URLComponents(url: url, resolvingAgainstBaseURL: false) else {
            return nil
        }
        let path = components.percentEncodedPath
        guard path.hasPrefix("/") else { return nil }
        let name = String(path.dropFirst())
        guard isName(name) else { return nil }
        let value = components.percentEncodedQuery.map { $0.removingPercentEncoding ?? $0 } ?? "yes"
        return Request(name: name, value: value)
    }

    /// Writes each request as its file in `folder`, which it makes when missing; a file there is replaced, as the
    /// script's copy replaces it. An empty value is an empty file: no request (or, for "events", the emptied events).
    public static func write(_ requests: [Request], into folder: URL) throws {
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        for request in requests {
            let text = request.value.isEmpty ? "" : request.value + "\n"
            try Data(text.utf8).write(to: folder.appending(path: request.name), options: .atomic)
        }
    }

    private static func isName(_ name: String) -> Bool {
        (1...64).contains(name.count) && !name.hasPrefix("-") && name.unicodeScalars.allSatisfy { scalar in
            ("a"..."z").contains(scalar) || ("0"..."9").contains(scalar) || scalar == "-"
        }
    }
}
