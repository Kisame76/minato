import Foundation

/// The run folders under the app's tmp/benchmark (BenchmarkMode's <run>/ and SessionRecording's session-<package>/),
/// which tools/benchmark.sh and tools/record-session.sh fetch after the app has ended. Nothing else deletes them, and
/// devicectl cannot, so they piled up on the iPhone (about 10,600 entries in tmp on 2026-11-11, where a listing took
/// 18 s). A run removes the others at its start: a series runs one at a time, and the Mac fetched each folder before
/// the next run started.
public enum BenchFolders {
    /// Removes every entry of `base` but `keeping`, and returns how many went. A missing `base` removes nothing.
    @discardableResult
    public static func removeOthers(in base: URL, keeping: String) -> Int {
        let files = FileManager.default
        guard let names = try? files.contentsOfDirectory(atPath: base.path) else { return 0 }
        var removed = 0
        for name in names where name != keeping {
            if (try? files.removeItem(at: base.appending(path: name))) != nil { removed += 1 }
        }
        return removed
    }
}
