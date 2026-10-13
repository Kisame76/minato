import Foundation

/// Which native libraries of an app cannot load (ADR-0055, step a3): those the converter's check refuses, those that
/// need a name nothing gives, and, since a library cannot load when one it needs cannot, those that need such a
/// library.
enum NativeFindings {
    /// What is known of one library of the app.
    struct Library {
        var name: String
        /// The converter's reasons (ADR-0055 a1); none when it takes the file or was not asked.
        var refusals: [String]
        /// The names its dynamic section asks the linker for (DT_NEEDED).
        var needed: [String]
    }

    /// NDK libraries that Android 15 has and Minato's guest does not (compatibility plan, gap 14).
    static let ndkLibrariesMinatoLacks: Set<String> = [
        "libamidi.so", "libbinder_ndk.so", "libneuralnetworks.so", "libOpenMAXAL.so", "libstdc++.so", "libsync.so",
    ]

    /// The libraries that cannot load, each with its reasons: its own first, then one for each library it needs that
    /// cannot load. A NEEDED name counts only when `publicLibraries`, the guest's list, is known: then a name that is
    /// neither a library of the app nor on the list is missing.
    static func failing(_ libraries: [Library], publicLibraries: Set<String>?) -> [String: [String]] {
        let names = Set(libraries.map(\.name))
        var reasons: [String: [String]] = [:]
        for library in libraries {
            var own = library.refusals
            if let publicLibraries {
                for name in library.needed where !names.contains(name) && !publicLibraries.contains(name) {
                    own.append(ndkLibrariesMinatoLacks.contains(name)
                        ? "it needs \(name), an NDK library Minato does not have yet (gap 14)"
                        : "it needs \(name), which is not in the app and not one of Minato's libraries")
                }
            }
            if !own.isEmpty { reasons[library.name] = own }
        }
        // A library that needs one that cannot load cannot load: a worklist over the reverse NEEDED edges, each library
        // taken once, so that the work grows with the libraries and their names, not with their product.
        var dependents: [String: [String]] = [:]
        for library in libraries {
            for name in Set(library.needed) where name != library.name && names.contains(name) {
                dependents[name, default: []].append(library.name)
            }
        }
        var pending = libraries.map(\.name).filter { reasons[$0] != nil }
        var next = 0
        while next < pending.count {
            let failed = pending[next]
            next += 1
            for dependent in dependents[failed] ?? [] {
                if reasons[dependent] == nil { pending.append(dependent) }
                reasons[dependent, default: []].append("it needs \(failed), which cannot load")
            }
        }
        return reasons
    }

    /// `seeds` and every library of the app they need, directly or through others.
    static func closure(of seeds: Set<String>, over libraries: [Library]) -> Set<String> {
        let byName = Dictionary(libraries.map { ($0.name, $0) }, uniquingKeysWith: { first, _ in first })
        var found: Set<String> = []
        var pending = seeds.filter { byName[$0] != nil }
        while let name = pending.popFirst() {
            guard found.insert(name).inserted else { continue }
            for needed in byName[name]?.needed ?? [] where byName[needed] != nil && !found.contains(needed) {
                pending.insert(needed)
            }
        }
        return found
    }

    /// The names in a `public.libraries.txt` (the guest's list): one per line, a comment from `#`, an option after the
    /// name (`nopreload`) ignored.
    static func publicLibraries(from text: String) -> Set<String> {
        var names: Set<String> = []
        for line in text.split(whereSeparator: \.isNewline) {
            let content = line.split(separator: "#", maxSplits: 1, omittingEmptySubsequences: false).first ?? ""
            if let name = content.split(whereSeparator: \.isWhitespace).first { names.insert(String(name)) }
        }
        return names
    }
}
