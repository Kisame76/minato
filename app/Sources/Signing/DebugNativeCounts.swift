#if DEBUG
import APKConvert
import Foundation

/// Debug builds only, a test aid for an import of an app with native code and no feature of the released app: the
/// numbers of what the converter did to an installed app's libraries, for one line in Get Info. It reads the ELF
/// originals that stay in the version's lib/ folder and asks the converter's apkconv_counts_of for each. Numbers only:
/// no library name, no symbol, no address, so a screenshot of the line names nothing.
enum DebugNativeCounts {
    /// apkconv_counts_of's form, so that a check can hand over numbers without a library.
    typealias Count = @Sendable (URL) -> apkconv_counts?

    /// The line for the libraries in `folder`, or nil when it holds none that count: every file of it that is not a
    /// container (a name ending in .dylib), each counted once.
    static func line(libraries folder: URL, count: Count = { counted($0) }) -> String? {
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        let originals = files.filter { $0.pathExtension != "dylib" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
        var total = apkconv_counts()
        var libraries = 0
        for file in originals {
            guard let found = count(file) else { continue }
            libraries += 1
            total.svc_rewritten += found.svc_rewritten
            total.svc_data += found.svc_data
            total.svc_in_data += found.svc_in_data
            total.undefined_words += found.undefined_words
            total.x18_islands += found.x18_islands
            total.renames_by_symbol += found.renames_by_symbol
            total.renames_by_unwind += found.renames_by_unwind
        }
        guard libraries > 0 else { return nil }
        return text(total, libraries: libraries)
    }

    /// The same line, counted on a thread of its own: a library can be over 100 MB and the converter reads it whole.
    static func lineOffMain(libraries folder: URL, count: @escaping Count = { counted($0) }) async -> String? {
        await Task.detached(priority: .utility) { line(libraries: folder, count: count) }.value
    }

    static func text(_ total: apkconv_counts, libraries: Int) -> String {
        "\(libraries) \(libraries == 1 ? "library" : "libraries"): "
            + "svc \(total.svc_rewritten) rewritten, \(total.svc_data) kept as data, "
            + "\(total.svc_in_data) in proven data; "
            + "FEAT_LSUI kept \(total.undefined_words); x18 islands \(total.x18_islands); "
            + "whole-function renames \(total.renames_by_symbol) by symbol, \(total.renames_by_unwind) by unwind entry"
    }

    private static func counted(_ file: URL) -> apkconv_counts? {
        var found = apkconv_counts()
        var error = [CChar](repeating: 0, count: 256)
        return apkconv_counts_of(file.path, &found, &error, error.count) == 0 ? found : nil
    }
}
#endif
