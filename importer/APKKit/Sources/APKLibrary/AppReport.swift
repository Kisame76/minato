import APKKit
import Foundation

/// The app report of Report a Problem (ADR-0070, the App's parts 2 and 3): plain text about Minato, the iPhone and
/// the app, its status and compatibility issues in Get Info's words, how its last session ended and that session's
/// kept log, and the URL of the issue form `app-report.yml` with the fields it can fill. The report itself goes by
/// the clipboard, as a URL holds only a few KB, and the form's source stays the user's. Nothing names a path.
public struct AppReport: Sendable, Equatable, Identifiable {
    public static let form = "https://github.com/Kisame76/minato/issues/new"

    /// Options of the form's "What happens".
    public enum What: String, Sendable, CaseIterable {
        case incompatible = "The import fails or the app shows as incompatible"
        case doesNotStart = "It does not start"
        case crashes = "It crashes"
    }

    /// Options of the form's "File format".
    public enum Format: String, Sendable, CaseIterable {
        case apk = ".apk"
        case apks = ".apks"
        case xapk = ".xapk"
        case apkm = ".apkm"
        case splitAPKs = "Split APKs (base.apk and split_*.apk)"
        case aab = ".aab"
        case withOBBs = "With OBB files"
    }

    public struct Device: Sendable, Equatable {
        /// MinatoVersion's text.
        public var minato: String
        /// The model identifier, hw.machine: "iPhone16,2".
        public var model: String
        public var ios: String

        public init(minato: String, model: String, ios: String) {
            self.minato = minato
            self.model = model
            self.ios = ios
        }
    }

    /// How the app's last session ended.
    public enum Ending: Sendable, Equatable {
        /// Linux's signal number, with its name.
        case signal(Int32, name: String?)
        case startFailed(String)
        case endedByIOS
        case exitStatus(Int32)
        /// The app closed itself this many seconds after it opened, as Get Info's footer says (ClosedItselfStore).
        case closedItself(seconds: Int)
    }

    public var entry: LibraryEntry
    public var device: Device
    /// Get Info's status line.
    public var status: String
    /// Every compatibility issue in Get Info's words.
    public var issues: [String]
    public var ending: Ending?
    /// The kept lines of the last session's guest log.
    public var log: String?
    /// The runtime's record of the end: its cause, registers and frames (LastSessionNotice.record).
    public var record: [String]

    public init(entry: LibraryEntry, device: Device, status: String, issues: [String], ending: Ending?,
                log: String?, record: [String] = []) {
        self.entry = entry
        self.device = device
        self.status = status
        self.issues = issues
        self.ending = ending
        self.log = log
        self.record = record
    }

    public var id: String { entry.id }

    public var what: What? {
        switch entry.state {
        case .incompatible, .needsSplits, .invalid: return .incompatible
        case .ready: break
        }
        switch ending {
        case .signal: return .crashes
        case .startFailed: return .doesNotStart
        case .endedByIOS, .exitStatus, .closedItself, nil: return nil
        }
    }

    /// A container by its kind, else OBBs, else a split set, else one APK; nil for a file that was no APK.
    public var format: Format? {
        switch (entry.source.fileName as NSString).pathExtension.lowercased() {
        case "apks": return .apks
        case "xapk": return .xapk
        case "apkm": return .apkm
        case "aab": return .aab
        case let kind:
            if !entry.obbs.isEmpty { return .withOBBs }
            if !entry.splits.isEmpty { return .splitAPKs }
            return entry.base != nil || kind == "apk" ? .apk : nil
        }
    }

    private var app: String { entry.report?.label ?? entry.displayName }

    private var package: String? { entry.id.hasPrefix("file:") ? nil : entry.id }

    private var version: String? {
        guard let report = entry.report else { return nil }
        guard let name = report.versionName, !name.isEmpty else { return "\(report.versionCode)" }
        return name
    }

    private var endingLine: String? {
        switch ending {
        case .signal(let number, let name): "it ended with signal \(number)" + (name.map { " (\($0))" } ?? "")
        case .startFailed(let reason): "it could not start: \(reason)"
        case .endedByIOS: "iOS ended it, maybe because it used too much memory"
        case .exitStatus(let status): "it ended with status \(status)"
        case .closedItself(let seconds): "it closed itself \(seconds) second\(seconds == 1 ? "" : "s") after it opened"
        case nil: nil
        }
    }

    /// Everything but the log, which the sheet shows under its own heading.
    public var summary: String {
        var lines = ["Minato: \(device.minato)", "iPhone: \(device.model), iOS \(device.ios)", "", "App: \(app)"]
        if let package { lines.append("Package: \(package)") }
        if let report = entry.report {
            let code = "\(report.versionCode)"
            lines.append("Version: " + (version == code ? code : "\(version ?? "") (\(code))"))
            lines.append("SDK: min \(report.minSdk), target \(report.targetSdk)")
            lines.append("ABIs: \(report.abis.isEmpty ? "none" : report.abis.joined(separator: ", "))")
            switch report.runtime {
            case .nativeActivity(let library):
                lines.append("Runtime: NativeActivity (\(library))" + (report.startsOnJavaPath ? " with Java" : ""))
            case .art: lines.append("Runtime: ART (Java and Kotlin)")
            }
            if let engine = report.engine { lines.append("Engine: \(engine)") }
            if let libraries = report.javaLibraries, !libraries.isEmpty {
                lines.append("Java loads: \(libraries.joined(separator: ", "))")
            }
        }
        if let format { lines.append("Format: \(format.rawValue)") }
        lines.append("Status: \(status)")
        if !issues.isEmpty {
            lines.append("Issues:")
            lines += issues.map { "- " + $0.replacingOccurrences(of: "\n", with: "\n  ") }
        }
        if let endingLine {
            lines.append("Last session: \(endingLine)")
            lines += record.map { "  " + $0 }
        }
        return lines.joined(separator: "\n") + "\n"
    }

    public var text: String { Self.text(summary: summary, log: log) }

    /// The report from its two parts, as the user may have edited them.
    public static func text(summary: String, log: String?) -> String {
        guard let log, !log.isEmpty else { return summary }
        return summary + "\nLog of the last session\n" + log + (log.hasSuffix("\n") ? "" : "\n")
    }

    /// The form with app, package, version, minato, device and ios, and what, format and details where known.
    public var url: URL {
        var fields: [(String, String)] = [("template", "app-report.yml"), ("app", app)]
        if let package { fields.append(("package", package)) }
        if let version { fields.append(("version", version)) }
        fields += [("minato", device.minato), ("device", device.model), ("ios", device.ios)]
        if let what { fields.append(("what", what.rawValue)) }
        if let format { fields.append(("format", format.rawValue)) }
        let details = ["Minato shows: \(status)", endingLine.map { "Last session: \($0)" }].compactMap { $0 }
        fields.append(("details", details.joined(separator: "\n")))
        let query = fields.map { "\(Self.encoded($0.0))=\(Self.encoded($0.1))" }.joined(separator: "&")
        return URL(string: "\(Self.form)?\(query)")!
    }

    /// Everything but ASCII letters, digits and -._~ percent-encoded, so that "+" stays a plus and "&" no field's end.
    private static func encoded(_ value: String) -> String {
        value.addingPercentEncoding(withAllowedCharacters: unreserved) ?? ""
    }

    private static let unreserved = CharacterSet(
        charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")
}
