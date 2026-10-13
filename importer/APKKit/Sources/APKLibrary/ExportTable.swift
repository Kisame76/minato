import Foundation

/// The guest's export table (ADR-0055, step a4), which Runtime makes at build time from the libraries the guest system
/// embeds: per library its NEEDED names and its exported symbols with their versions, and which are stubs. The format:
///
///     # minato export table 1
///     @ libandroid.so
///     needs libc.so libnativewindow.so
///     AAsset_close@@LIBANDROID
///     ASensorManager_getInstance@@LIBANDROID!
///
/// `name@@V` is the default version of the symbol, `name@V` a hidden one, a plain `name` an unversioned or base-version
/// one; a trailing `!` marks a stub, which logs once and answers nothing. A header line `@ libsigchain.so global`
/// marks a library of the linker's global group, whose names bind for every library without a NEEDED on it.
struct ExportTable: Sendable, Equatable {
    struct Definition: Sendable, Equatable {
        var version: String?
        var isDefault: Bool
        var isStub: Bool
    }

    struct Library: Sendable, Equatable {
        var needs: [String] = []
        var isGlobal = false
        var symbols: [String: [Definition]] = [:]
        /// The versions it defines, collected when the table is read.
        var versions: Set<String> = []

        /// Whether the library has version tables: any of its symbols has a version.
        var hasVersions: Bool { !versions.isEmpty }
    }

    struct FormatError: Error, Equatable, CustomStringConvertible {
        var line: Int
        var reason: String
        var description: String { "export table, line \(line): \(reason)" }
    }

    static let header = "# minato export table 1"

    var libraries: [String: Library]
    /// For each symbol name the libraries that define it, made when the table is read.
    private(set) var definers: [String: [String]] = [:]

    init(text: String) throws {
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        guard lines.first == Self.header else {
            throw FormatError(line: 1, reason: "not a Minato export table, version 1")
        }
        var libraries: [String: Library] = [:]
        var definers: [String: [String]] = [:]
        var current: String?
        var needsSeen = false
        for (index, line) in lines.enumerated().dropFirst() {
            let number = index + 1
            if line.isEmpty && index == lines.count - 1 { break }
            guard !line.isEmpty, !line.unicodeScalars.contains(where: { $0.value < 0x20 || $0.value == 0x7F }) else {
                throw FormatError(line: number, reason: "an empty line or a control character")
            }
            if line.hasPrefix("#") { continue }
            if line.hasPrefix("@ ") {
                var words = line.dropFirst(2).split(separator: " ", omittingEmptySubsequences: false)
                let isGlobal = words.count == 2 && words[1] == "global"
                if isGlobal { words.removeLast() }
                let name = words.count == 1 ? String(words[0]) : ""
                guard !name.isEmpty, libraries[name] == nil else {
                    throw FormatError(line: number, reason: "a library name that is empty, has a space, or comes twice")
                }
                libraries[name] = Library(isGlobal: isGlobal)
                current = name
                needsSeen = false
                continue
            }
            guard let current else { throw FormatError(line: number, reason: "a line outside a library block") }
            if line == "needs" || line.hasPrefix("needs ") {
                guard !needsSeen, libraries[current]?.symbols.isEmpty == true else {
                    throw FormatError(line: number, reason: "needs comes once, first in its block")
                }
                needsSeen = true
                libraries[current]?.needs = line.split(separator: " ").dropFirst().map(String.init)
                continue
            }
            guard needsSeen, !line.contains(" ") else {
                throw FormatError(line: number, reason: "a symbol line is malformed")
            }
            var text = Substring(line)
            let isStub = text.hasSuffix("!")
            if isStub { text = text.dropLast() }
            var name = text
            var version: String?
            var isDefault = true
            if let at = text.firstIndex(of: "@") {
                name = text[..<at]
                var rest = text[text.index(after: at)...]
                if rest.hasPrefix("@") {
                    rest = rest.dropFirst()
                } else {
                    isDefault = false
                }
                guard !rest.isEmpty, !rest.contains("@") else {
                    throw FormatError(line: number, reason: "a bad version")
                }
                version = String(rest)
            }
            guard !name.isEmpty else { throw FormatError(line: number, reason: "a symbol without a name") }
            libraries[current]?.symbols[String(name), default: []].append(
                Definition(version: version, isDefault: isDefault, isStub: isStub))
            if let version { libraries[current]?.versions.insert(version) }
            if libraries[current]?.symbols[String(name)]?.count == 1 {
                definers[String(name), default: []].append(current)
            }
        }
        self.libraries = libraries
        self.definers = definers
    }
}
