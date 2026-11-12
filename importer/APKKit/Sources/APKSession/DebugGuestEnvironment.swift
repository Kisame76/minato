import Foundation

/// The guest environment a Debug build takes from a file in Minato's container (ADR-0030 V5's data step), such
/// as libvulkan's MINATO_VULKAN_BGRA8_FIRST=1: one KEY=VALUE per line, "#" starts a comment. Only MINATO_*
/// keys pass, so the file cannot change what the runtime sets (BOOTCLASSPATH, PATH, ANDROID_*); every other
/// line that is not blank is skipped and named for the log, by its key or, without one, as it is.
public enum DebugGuestEnvironment {
    public struct Parsed: Sendable, Equatable {
        /// The entries for the guest, KEY=VALUE, in the file's order.
        public var entries: [String]
        /// The skipped lines' keys, or the lines themselves when they have no key.
        public var skipped: [String]

        public init(entries: [String], skipped: [String]) {
            self.entries = entries
            self.skipped = skipped
        }
    }

    public static let prefix = "MINATO_"

    public static func parse(_ text: String) -> Parsed {
        var parsed = Parsed(entries: [], skipped: [])
        for raw in text.split(omittingEmptySubsequences: true, whereSeparator: \.isNewline) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            guard !line.isEmpty, !line.hasPrefix("#") else { continue }
            guard let sign = line.firstIndex(of: "="), sign != line.startIndex else {
                parsed.skipped.append(line)
                continue
            }
            let key = String(line[..<sign])
            if isAllowed(key) {
                parsed.entries.append(line)
            } else {
                parsed.skipped.append(key)
            }
        }
        return parsed
    }

    /// Sets each entry in this process's environment too (KEY=VALUE, the value after the first "="): Minato's own
    /// switches, such as MINATO_AUDIO_MUTE, are read with the host's getenv, and the entries otherwise reach only the
    /// guest's environment.
    public static func exportToHost(_ entries: [String]) {
        for entry in entries {
            guard let sign = entry.firstIndex(of: "=") else { continue }
            setenv(String(entry[..<sign]), String(entry[entry.index(after: sign)...]), 1)
        }
    }

    /// MINATO_ and at least one more character, all of them capitals, digits or "_", as a shell name.
    private static func isAllowed(_ key: String) -> Bool {
        key.hasPrefix(prefix) && key.count > prefix.count
            && key.allSatisfy { ("A"..."Z").contains($0) || ("0"..."9").contains($0) || $0 == "_" }
    }
}
