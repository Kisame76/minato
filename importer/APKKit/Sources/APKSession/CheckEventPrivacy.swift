import APKLibrary

/// The Debug build's check events (tmp/session-check/events, written while a check's folder is there) name none of
/// the user's apps. That folder can stay on a device after a check, so its events must hold nothing of the apps
/// someone uses: the names, labels, packages and files of any app but Minato's own test apps are cut from every line.
public enum CheckEventPrivacy {
    /// The packages of Minato's own test apps (fixtures, benchmarks, the check apps), which the checks name.
    public static let ownPackages = "io.github.kisame76.minato."

    /// What `entry` is known by, to cut from the events: none for Minato's own test apps. A one-letter name names
    /// nothing and would cut every line, so it stays.
    public static func names(of entry: LibraryEntry) -> [String] {
        let package = entry.id.hasPrefix("file:") ? String(entry.id.dropFirst(5)) : entry.id
        return names(package: package, others: [entry.id, entry.displayName, entry.source.fileName])
    }

    /// What an app of `package` is known by, with its `others` (name, file): none for Minato's own test apps.
    public static func names(package: String, others: [String] = []) -> [String] {
        guard !package.hasPrefix(ownPackages) else { return [] }
        return ([package] + others).filter { $0.count > 1 }
    }

    /// `line` with every one of `names` as "<app>", the longest first so that no part of a longer one stays.
    public static func redact(_ line: String, names: some Collection<String>) -> String {
        names.sorted { $0.count > $1.count }.reduce(line) { $0.replacingOccurrences(of: $1, with: "<app>") }
    }
}

extension LastSessionNotice {
    /// The check event's words for the end, after "process library quitUnexpectedly": the cause only, never the
    /// app's name or package.
    public var checkEvent: String {
        switch cause {
        case .signal(let number): "signal \(number)"
        case .failure(let reason): "failure \(reason)"
        case .endedByIOS: "endedByIOS"
        case .exitStatus(let status): "status \(status)"
        }
    }
}
