import Foundation

/// A unit over the cap of ADR-0048 decision 1.
public enum ImportUnitError: Error, Sendable, Equatable, LocalizedError {
    case tooManyFiles(Int)

    /// A reason for the app to show after "could not be added:".
    public var errorDescription: String? {
        switch self {
        case .tooManyFiles(let count): "it has \(count) files, and Minato takes at most \(ImportUnit.maxFiles) at a time"
        }
    }
}

/// One pick, several files or a folder, imported together (ADR-0048, decision 1).
public enum ImportUnit {
    /// [guess] Enough for a full set of language splits; tuned with real files.
    public static let maxFiles = 512
    /// Folders inside folders that Minato follows before it gives up.
    public static let maxDepth = 16

    /// The files a picked folder gives: its `.apk` and `.obb` files, sorted by name; or, when it holds nothing but
    /// one folder, that folder's, as Android's directory parser does (`ApkLiteParseUtils.java:179-181`). Hidden
    /// files such as `.DS_Store` do not count, and links are not followed.
    public static func files(inFolder folder: URL) throws -> [URL] {
        let keys: [URLResourceKey] = [.isDirectoryKey, .isRegularFileKey, .isSymbolicLinkKey]
        var current = folder
        for _ in 0..<maxDepth {
            let entries = try FileManager.default.contentsOfDirectory(at: current, includingPropertiesForKeys: keys,
                                                                      options: [.skipsHiddenFiles])
            if entries.count == 1, let only = entries.first,
               let values = try? only.resourceValues(forKeys: Set(keys)),
               values.isDirectory == true, values.isSymbolicLink != true {
                current = only
                continue
            }
            let files = entries.filter { entry in
                guard ["apk", "obb"].contains(entry.pathExtension.lowercased()),
                      let values = try? entry.resourceValues(forKeys: Set(keys)) else { return false }
                return values.isRegularFile == true && values.isSymbolicLink != true
            }
            guard files.count <= maxFiles else { throw ImportUnitError.tooManyFiles(files.count) }
            return files.sorted { $0.lastPathComponent.utf8.lexicographicallyPrecedes($1.lastPathComponent.utf8) }
        }
        return []
    }
}
