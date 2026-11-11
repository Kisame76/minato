import Foundation
import Testing

/// No installed app may show in the device's system log (a beta user's sysdiagnose must not list their apps): a value
/// that can name an app is never logged with `privacy: .public`, and no NSLog, print or debugPrint in the app's sources
/// or the libraries it links names one. A source scan, as the app target has no tests of its own.
///
/// What it sees: an interpolation `\(expression, privacy: .public)` or an NSLog/print/debugPrint argument whose
/// expression holds one of `nameWords` (an app's package, entry id, display name, identifier, tag, file name, path or
/// URL, an error's text that may embed a path, a guest's line, a reason or failure text). What it cannot see: a value
/// whose expression looks like none of them (a renamed local); those are checked by hand at each landing.
@Suite("LogPrivacyScan")
struct LogPrivacyScanTests {
    /// The repository's root, from this file's place (importer/APKKit/Tests/APKSessionTests/).
    static let root = URL(filePath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()

    /// The sources the app ships: the app's own and the libraries it links (not the Mac's command-line tools).
    static let shipped = ["app/Sources"] + ["APKKit", "APKLibrary", "APKSession", "APKFormats", "APKSigning",
                                            "APKBench", "APKPack"].map { "importer/APKKit/Sources/\($0)" }

    static let nameWords = #"\b(entry|package|packageName|displayName|name|names|title|identifier|tag|path|"#
        + #"lastPathComponent|url|URL|fileName|file|localizedDescription|line|reason|failure|decisions)\b"#

    static func swiftFiles() -> [URL] {
        shipped.flatMap { folder -> [URL] in
            let base = root.appending(path: folder)
            let walker = FileManager.default.enumerator(at: base, includingPropertiesForKeys: nil)
            return (walker?.allObjects as? [URL] ?? []).filter { $0.pathExtension == "swift" }
        }
    }

    /// Every `\(expression, privacy: .public)` of `text`, as "file:line: expression".
    static func publicValues(in text: String, file: String) -> [String] {
        let pattern = #"\\\(((?:[^()]|\([^()]*\))*?),\s*privacy:\s*\.public\)"#
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return [] }
        var found: [String] = []
        for (number, line) in text.split(separator: "\n", omittingEmptySubsequences: false).enumerated() {
            let line = String(line)
            for match in regex.matches(in: line, range: NSRange(line.startIndex..., in: line)) {
                guard let range = Range(match.range(at: 1), in: line) else { continue }
                found.append("\(file):\(number + 1): \(line[range])")
            }
        }
        return found
    }

    static func namesAnApp(_ expression: String) -> Bool {
        expression.range(of: nameWords, options: .regularExpression) != nil
    }

    @Test func noPublicLogValueNamesAnApp() throws {
        var offending: [String] = []
        for file in Self.swiftFiles() {
            let text = try String(contentsOf: file, encoding: .utf8)
            let place = file.path.replacingOccurrences(of: Self.root.path + "/", with: "")
            offending += Self.publicValues(in: text, file: place).filter {
                Self.namesAnApp(String($0.split(separator: ":", maxSplits: 2)[2]))
            }
        }
        #expect(offending.isEmpty, "public log values that can name an app:\n\(offending.joined(separator: "\n"))")
    }

    @Test func noPrintNamesAnApp() throws {
        var offending: [String] = []
        let call = #"\b(NSLog|print|debugPrint)\((.*)$"#
        for file in Self.swiftFiles() {
            let text = try String(contentsOf: file, encoding: .utf8)
            let place = file.path.replacingOccurrences(of: Self.root.path + "/", with: "")
            for (number, line) in text.split(separator: "\n", omittingEmptySubsequences: false).enumerated() {
                guard let range = line.range(of: call, options: .regularExpression) else { continue }
                if Self.namesAnApp(String(line[range])) { offending.append("\(place):\(number + 1): \(line)") }
            }
        }
        #expect(offending.isEmpty, "prints that can name an app:\n\(offending.joined(separator: "\n"))")
    }

    /// The scan itself: it finds a public package and passes a public count or a private package.
    @Test func theScanSeesWhatItShould() {
        let lines = #"""
            log.info("\(self.entry.id, privacy: .public): started")
            log.info("\(count, privacy: .public) frames")
            log.info("\(entry.id, privacy: .private(mask: .hash)) gone")
            log.error("\(url.lastPathComponent, privacy: .public) failed: \(error.localizedDescription, privacy: .public)")
            """#
        let found = Self.publicValues(in: lines, file: "x").map { String($0.split(separator: ":", maxSplits: 2)[2]) }
        #expect(found.map { $0.trimmingCharacters(in: .whitespaces) }
            == ["self.entry.id", "count", "url.lastPathComponent", "error.localizedDescription"])
        #expect(found.filter(Self.namesAnApp).count == 3)
    }
}
