/// An app's request to remove an app (APKRT_REQUEST_UNINSTALL, ADR-0077): the text "<target package>\n<asking
/// package>". The host takes only the target from it; the asking app is its own session's, never the text's.
public enum UninstallRequest {
    /// The target package, the text's first line, when it is a package name as Android's PackageParser takes one: two
    /// or more segments, each an ASCII letter followed by letters, digits or underscores. nil for anything else, which
    /// names no app of the library (a file entry's "file:" id among it).
    public static func target(of text: String) -> String? {
        let first = text.split(separator: "\n", maxSplits: 1, omittingEmptySubsequences: false).first.map(String.init)
        guard let name = first else { return nil }
        let segments = name.split(separator: ".", omittingEmptySubsequences: false)
        guard segments.count >= 2, segments.allSatisfy(isSegment) else { return nil }
        return name
    }

    private static func isSegment(_ segment: Substring) -> Bool {
        guard let head = segment.unicodeScalars.first, isLetter(head) else { return false }
        return segment.unicodeScalars.allSatisfy { isLetter($0) || ("0"..."9").contains($0) || $0 == "_" }
    }

    private static func isLetter(_ scalar: Unicode.Scalar) -> Bool {
        ("a"..."z").contains(scalar) || ("A"..."Z").contains(scalar)
    }
}
