import APKKit
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
        /// Undefined symbols that nothing it needs defines (ADR-0055 a4); it cannot load.
        var missingSymbols: [String] = []
        /// Undefined symbols that only a stub defines: it loads, and calls something that answers nothing.
        var stubSymbols: [String] = []
    }

    /// A library of the app as the symbols of another see it.
    struct AppLibrary: Equatable {
        var defined: Set<String>
        var needed: [String]
        /// Its symbols could not be read: it counts as defining whatever is asked.
        var isUnknown = false
    }

    /// What binding a library's undefined symbols found.
    struct Binding: Equatable {
        var missing: [String] = []
        var stubs: [String] = []
    }

    /// Binds the undefined symbols of the app's libraries as linker64 does in an app's isolated namespace, for all
    /// the libraries at once. A library loads with the tree of whichever library `System.loadLibrary` named first, and
    /// bionic links it against the global group (the table's `global` libraries) and the local group of that root,
    /// which walks the app's libraries breadth first and goes one hop into the guest: a public library an app library
    /// needs is searched, and what it needs only when some app library, of any part, needs it too. Any app library can be the root, so a symbol counts as
    /// missing only when no such group has it. The groups of all roots that reach a library are taken as the one group
    /// of its weakly connected part of the app's NEEDED graph: that finds a symbol a little more often than Android
    /// does (a library only a sibling's root reaches), never less, so it never blocks an app Android loads, and it
    /// costs one pass over the libraries and one lookup for each reference.
    struct Binder {
        private struct Part {
            var defined: Set<String> = []
            var isUnknown = false
            var system: Set<String> = []
        }

        private let table: ExportTable
        private let global: Set<String>
        private var partOf: [String: Int] = [:]
        private var parts: [Part] = []

        init(app: [String: AppLibrary], table: ExportTable) {
            self.table = table
            global = Set(table.libraries.filter { $0.value.isGlobal }.keys)
            // Union-find over the app's libraries by their NEEDED names that are the app's own.
            let names = app.keys.sorted()
            var index: [String: Int] = [:]
            for (position, name) in names.enumerated() { index[name] = position }
            var parent = Array(0..<names.count)
            func find(_ node: Int) -> Int {
                var node = node
                while parent[node] != node {
                    parent[node] = parent[parent[node]]
                    node = parent[node]
                }
                return node
            }
            for (position, name) in names.enumerated() {
                for needed in app[name]?.needed ?? [] {
                    if let other = index[needed] { parent[find(position)] = find(other) }
                }
            }
            var partIndex: [Int: Int] = [:]
            for (position, name) in names.enumerated() {
                let root = find(position)
                let number = partIndex[root] ?? parts.count
                if partIndex[root] == nil {
                    partIndex[root] = number
                    parts.append(Part())
                }
                partOf[name] = number
                guard let library = app[name] else { continue }
                if library.isUnknown { parts[number].isUnknown = true }
                parts[number].defined.formUnion(library.defined)
                for needed in library.needed where index[needed] == nil { parts[number].system.insert(needed) }
            }
            // bionic admits a system library's dependency when any library of the app namespace, of any part, needs
            // it directly (is_accessible(soinfo*)): a system library's own needs are followed through those only.
            let reachable = parts.reduce(into: Set<String>()) { $0.formUnion($1.system) }
            for number in parts.indices {
                var queue = Array(parts[number].system)
                var next = 0
                while next < queue.count {
                    let name = queue[next]
                    next += 1
                    for dependency in table.libraries[name]?.needs ?? []
                    where reachable.contains(dependency) && parts[number].system.insert(dependency).inserted {
                        queue.append(dependency)
                    }
                }
            }
        }

        /// What the undefined symbols of the app library `name` bind to; `own` is what the library defines itself.
        func bind(_ name: String, references: [ELFSymbols.Reference], own: Set<String>) -> Binding {
            var binding = Binding()
            let part = partOf[name].map { parts[$0] } ?? Part()
            for reference in references where !reference.isWeak {
                if own.contains(reference.name) || part.isUnknown || part.defined.contains(reference.name) { continue }
                var found = false
                var real = false
                for library in table.definers[reference.name] ?? []
                where part.system.contains(library) || global.contains(library) {
                    guard let table = table.libraries[library],
                          let definition = NativeFindings.definition(of: reference, in: table) else { continue }
                    found = true
                    if !definition.isStub { real = true }
                }
                if !found { binding.missing.append(reference.name) } else if !real { binding.stubs.append(reference.name) }
            }
            return binding
        }
    }

    /// The definition in `library` that `reference` binds to (Runtime's rules, from bionic's linker), if any.
    fileprivate static func definition(of reference: ELFSymbols.Reference, in library: ExportTable.Library)
        -> ExportTable.Definition? {
        guard let definitions = library.symbols[reference.name] else { return nil }
        guard let version = reference.version else {
            return definitions.first { $0.isDefault }
        }
        if library.versions.contains(version) { return definitions.first { $0.version == version } }
        if library.hasVersions { return definitions.first { $0.version == nil } }
        return definitions.first
    }

    /// NDK libraries that Android 15 has and Minato's guest does not.
    static let ndkLibrariesMinatoLacks: Set<String> = [
        "libamidi.so", "libbinder_ndk.so", "libneuralnetworks.so", "libOpenMAXAL.so", "libstdc++.so", "libsync.so",
    ]

    /// The libraries that cannot load, each with its reasons: its own first, then one for each library it needs that
    /// cannot load, with that one's first reason as the cause. A NEEDED name counts only when `publicLibraries`, the
    /// guest's list, is known: then a name that is neither a library of the app nor on the list is missing.
    static func failing(_ libraries: [Library], publicLibraries: Set<String>?) -> [String: [String]] {
        let names = Set(libraries.map(\.name))
        var reasons: [String: [String]] = [:]
        for library in libraries {
            var own = library.refusals
            if let publicLibraries {
                for name in library.needed where !names.contains(name) && !publicLibraries.contains(name) {
                    own.append(ndkLibrariesMinatoLacks.contains(name)
                        ? "it needs \(name), an NDK library Minato does not have yet"
                        : "it needs \(name), which is not in the app and not one of Minato's libraries")
                }
            }
            if !library.missingSymbols.isEmpty {
                own.append("it needs \(list(library.missingSymbols)) that nothing it needs defines")
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
                // With the cause, the first reason of the one it needs, so that one line read out says why.
                let cause = reasons[failed]?.first.map { ": " + $0 } ?? ""
                reasons[dependent, default: []].append("it needs \(failed), which cannot load\(cause)")
            }
        }
        return reasons
    }

    /// For a library that stays: how many functions it calls that only a stub defines, and the first of them.
    static func stubReason(_ names: [String]) -> String {
        var seen: Set<String> = []
        let unique = names.filter { seen.insert($0).inserted }
        let shown = unique.prefix(5).joined(separator: ", ")
        let rest = unique.count > 5 ? " and \(unique.count - 5) more" : ""
        return "calls \(unique.count) function\(unique.count == 1 ? "" : "s") that Minato does not have yet "
            + "(they log once and answer nothing): \(shown)\(rest)"
    }

    /// "the symbol x" or "3 symbols: a, b, c and 1 more", for a reason.
    static func list(_ names: [String], limit: Int = 5) -> String {
        let shown = names.prefix(limit).joined(separator: ", ")
        let rest = names.count > limit ? " and \(names.count - limit) more" : ""
        return names.count == 1 ? "the symbol \(shown)" : "\(names.count) symbols (\(shown)\(rest))"
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
