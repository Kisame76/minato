import APKKit
import Foundation

/// What converts native code at import (ADR-0032): the converter, and the SHA-256 of the certificate it
/// signs with, which the entry records as `signedWith` (ADR-0023). Without a certificate the fingerprint
/// is nil: the converter then only checks that the code can run, and its containers go again, so the
/// verdict does not depend on having a certificate at import.
public struct ImportSigning: Sendable {
    public var converter: any ContainerConverting
    public var fingerprint: String?

    public init(converter: any ContainerConverting, fingerprint: String?) {
        self.converter = converter
        self.fingerprint = fingerprint
    }
}

/// A converter throws this for a library that cannot run on iOS as it is; the import then makes the
/// app incompatible with the reason (ADR-0032, decision 4).
public struct ConversionRefused: Error, Equatable, Sendable, LocalizedError {
    public var reason: String

    public init(reason: String) {
        self.reason = reason
    }

    /// The refusal in a message of APKConvert, which marks code that cannot run on iOS with
    /// "incompatible: " (apkconv.h); nil for any other message.
    public init?(apkConvertMessage message: String) {
        let mark = "incompatible: "
        guard message.hasPrefix(mark) else { return nil }
        self.init(reason: String(message.dropFirst(mark.count)))
    }

    public var errorDescription: String? { reason }
}

/// The converter's check mode for a library of the app (ADR-0055, step a1): every reason it would refuse it for, in
/// the converter's words; empty when it takes it. It needs no certificate, so the import asks it whether it can sign
/// or not.
public protocol ELFRefusing: Sendable {
    func refusals(of elf: URL) throws -> [String]
}

/// A library of the app that cannot run: refused by the converter, or damaged in the APK.
struct RefusedLibrary: Error {
    var library: String
    var reason: String
    /// The other libraries of the start path that are refused too, so that one refusal names them all.
    var also: [RefusedLibrary] = []
    /// The libraries outside the start path that are refused: they degrade, and the report names them too.
    var degraded: [(library: String, reason: String)] = []

    init(library: String, reason: String) {
        self.library = library
        self.reason = reason
    }

    /// Every finding of the refusal: its own library and the other blockers as refusals, then what degraded.
    var issues: [Compatibility.Issue] {
        ([self] + also).map { .conversionFailed(library: $0.library, reason: $0.reason) }
            + degraded.map { .libraryDegraded(library: $0.library, reason: $0.reason) }
    }

    /// For an error reading the library out of the APK: damage, which no second try repairs, or our own
    /// limit; nil for an error of reading the file, which may not happen again.
    init?(readError: any Error, library: String) {
        switch readError as? APKError {
        case .truncated, .crcMismatch, .inconsistentEntry, .encryptedEntry, .unsupportedCompression:
            self.init(library: library, reason: "its data in the APK is damaged")
        case .entryTooLarge:
            self.init(library: library, reason: "it is larger than \(ZipArchive.defaultMaxEntrySize >> 20) MiB, which Minato cannot extract yet")
        default:
            return nil
        }
    }
}

extension InstalledNativeCode {
    /// The degrading findings of the check, as the report holds them.
    var findings: [Compatibility.Issue] {
        degraded.map { .libraryDegraded(library: $0.library, reason: $0.reason) }
            + stubCalls.map { .libraryCallsStubs(library: $0.library, reason: $0.reason) }
    }
}

/// What installing a version's native code gave: the certificate that signed it, and the libraries left out as
/// degraded.
struct InstalledNativeCode {
    var signedWith: String?
    var degraded: [(library: String, reason: String)]
    /// The libraries that stay and call functions only a stub of Minato's defines.
    var stubCalls: [(library: String, reason: String)] = []
    /// The digests of the lists the check used; nil where it ran without any.
    var checks: Compatibility.Checks?
}

extension Library {
    /// Extracts the app's arm64-v8a libraries, the ELF originals, into the version's lib/ and converts
    /// each into its container <name>.so.dylib (ADR-0008): those of the base and of every chosen split, each
    /// from its APK in `archives`, by its split's name, nil for the base (ADR-0048, decision 8). Returns the
    /// certificate that signed them all; nil without one or when one could not be signed, and then only the
    /// originals stay, for the first tap to sign. Throws `RefusedLibrary` for a library that cannot run.
    func installNativeCode(of report: APKReport, from archives: [String?: ZipArchive], into versionDirectory: URL,
                           signing: ImportSigning?) throws -> InstalledNativeCode {
        // Android's installer extracts only these and skips the others (NativeLibraryHelper).
        var libraries = report.nativeLibraries.filter { Self.isExtracted($0.name) }
        // Each name once: of one name in several APKs, the analysis keeps the copy Android installs (decision 8).
        // Names that differ only in case Android keeps apart, and so does the iPhone's case-sensitive volume, but one
        // lib/ on the Mac's or the Simulator's, which ignore case by default, cannot. They are refused everywhere, so
        // that device and Simulator agree, as split names are (decision 3). ASCII names: lowercasing folds them.
        var names: [String: String] = [:]
        for library in libraries {
            if library.name.utf8.count > Self.longestName {
                throw RefusedLibrary(library: library.name, reason: "its name is longer than \(Self.longestName) bytes")
            }
            if let other = names.updateValue(library.name, forKey: library.name.lowercased()), other != library.name {
                throw RefusedLibrary(library: library.name, reason: "its name differs from \(other) only in case")
            }
        }

        // The originals go into lib.partial/, which becomes lib/ once all are out: an error or the end of the
        // process halfway leaves no lib/ that the first tap would take as complete.
        let lib = versionDirectory.appendingPathComponent("lib", isDirectory: true)
        let partial = versionDirectory.appendingPathComponent("lib.partial", isDirectory: true)
        try? FileManager.default.removeItem(at: partial)
        try FileManager.default.createDirectory(at: partial, withIntermediateDirectories: true)
        var degradedNow: [(library: String, reason: String)] = []
        var stubsNow: [(library: String, reason: String)] = []
        let checksNow: Compatibility.Checks? =
            listDigests.exportTable == nil && listDigests.publicLibraries == nil ? nil : listDigests
        do {
            for library in libraries {
                let contents: Data
                do {
                    guard let archive = archives[library.split], let entry = archive.entry(named: library.path) else {
                        throw APKError.entryNotFound(library.path)
                    }
                    contents = try archive.data(for: entry)
                } catch {
                    if let refusal = RefusedLibrary(readError: error, library: library.name) { throw refusal }
                    throw error
                }
                try contents.write(to: partial.appendingPathComponent(library.name), options: .atomic)
            }
            // What cannot load is found on the extracted files, with or without a certificate: the converter's check
            // and the NEEDED names (ADR-0055 a1 and a3), and whatever needs a library that cannot load. A library of
            // the start path, or one it needs, blocks the app with every reason of every such library; any other is
            // left out of lib/ and degrades.
            var degraded: [(library: String, reason: String)] = []
            var stubs: [(library: String, reason: String)] = []
            defer {
                degradedNow = degraded
                stubsNow = stubs
            }
            if checker != nil || publicLibraries != nil || exportTable != nil {
                var facts: [NativeFindings.Library] = []
                var symbols: [String: ELFSymbols] = [:]
                for library in libraries {
                    let file = partial.appendingPathComponent(library.name)
                    // A file that cannot be read is no verdict, as for the converter: the first tap tries again.
                    let refusals = (try? checker?.refusals(of: file)) ?? []
                    let readsELF = publicLibraries != nil || exportTable != nil
                    let needed = readsELF ? ((try? ELFDynamic.needed(
                        in: Data(contentsOf: file, options: .mappedIfSafe))) ?? []) : []
                    if exportTable != nil, let read = try? ELFSymbols.read(
                        Data(contentsOf: file, options: .mappedIfSafe)) {
                        symbols[library.name] = read
                    }
                    facts.append(NativeFindings.Library(name: library.name, refusals: refusals, needed: needed))
                }
                if let exportTable {
                    // The app's own libraries define for each other; one whose symbols could not be read defines
                    // whatever is asked, so that it never makes another look broken.
                    var app: [String: NativeFindings.AppLibrary] = [:]
                    for fact in facts {
                        app[fact.name] = NativeFindings.AppLibrary(
                            defined: symbols[fact.name]?.defined ?? [], needed: fact.needed,
                            isUnknown: symbols[fact.name] == nil)
                    }
                    let binder = NativeFindings.Binder(app: app, table: exportTable)
                    for index in facts.indices {
                        guard let own = symbols[facts[index].name] else { continue }
                        let binding = binder.bind(facts[index].name, references: own.undefined, own: own.defined)
                        facts[index].missingSymbols = binding.missing
                        facts[index].stubSymbols = binding.stubs
                    }
                }
                let failing = NativeFindings.failing(facts, publicLibraries: publicLibraries)
                let blocking = NativeFindings.closure(of: StartPath.libraries(of: report), over: facts)
                var blockers: [RefusedLibrary] = []
                for library in libraries {
                    guard let reasons = failing[library.name] else { continue }
                    let text = reasons.joined(separator: "\n")
                    if blocking.contains(library.name) {
                        blockers.append(RefusedLibrary(library: library.name, reason: text))
                    } else {
                        degraded.append((library.name, text))
                        try? FileManager.default.removeItem(at: partial.appendingPathComponent(library.name))
                    }
                }
                if var first = blockers.first {
                    first.also = Array(blockers.dropFirst())
                    first.degraded = degraded
                    throw first
                }
                libraries.removeAll { library in degraded.contains { $0.library == library.name } }
                stubs = facts.filter { !$0.stubSymbols.isEmpty && failing[$0.name] == nil }.map {
                    ($0.name, NativeFindings.stubReason($0.stubSymbols))
                }
            }
            try? FileManager.default.removeItem(at: lib)
            try FileManager.default.moveItem(at: partial, to: lib)
        } catch {
            try? FileManager.default.removeItem(at: partial)
            throw error
        }
        let originals = libraries.map { lib.appendingPathComponent($0.name) }
        guard let signing else {
            return InstalledNativeCode(signedWith: nil, degraded: degradedNow, stubCalls: stubsNow, checks: checksNow)
        }

        // Every library is converted, also after one that could not be signed, so that none that cannot
        // run slips through.
        var signed = true
        for original in originals {
            do {
                try signing.converter.convert(original, to: original.appendingPathExtension("dylib"))
            } catch let refusal as ConversionRefused {
                var refused = RefusedLibrary(library: original.lastPathComponent, reason: refusal.reason)
                refused.degraded = degradedNow
                throw refused
            } catch {
                signed = false
            }
        }
        guard signed, let fingerprint = signing.fingerprint else {
            for original in originals {
                try? FileManager.default.removeItem(at: original.appendingPathExtension("dylib"))
            }
            return InstalledNativeCode(signedWith: nil, degraded: degradedNow, stubCalls: stubsNow, checks: checksNow)
        }
        return InstalledNativeCode(signedWith: fingerprint, degraded: degradedNow, stubCalls: stubsNow, checks: checksNow)
    }

    /// The longest library name Minato takes: lib/ holds <name>.dylib next to the library, which the converter writes
    /// through <name>.dylib.<8 hex digits>.tmp (c_api.cpp, replace_file), and a file name has at most 255 bytes.
    static let longestName = Int(NAME_MAX) - ".dylib".utf8.count - ".00000000.tmp".utf8.count

    /// The names that Android's installer extracts from lib/<abi>/: lib*.so of A-Z, a-z, 0-9 and
    /// + , - . = _ (isFilenameSafe).
    static func isExtracted(_ name: String) -> Bool {
        name.hasPrefix("lib") && name.hasSuffix(".so")
            && name.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || "+,-.=_".contains($0)) }
    }

    /// Makes sure the originals of a ready app's native code are in its version's lib/, a NativeActivity app's or a
    /// Java app's JNI libraries (ADR-0039 c5), extracting them from its base.apk and split APKs for an entry imported
    /// before the import did, or whose lib/ is gone; the first tap signs them (ADR-0032, decision 3). Returns how many
    /// there are. A library that cannot run makes the app incompatible, as at import.
    @discardableResult
    public func prepareNativeCode(id: String) throws -> Int {
        guard let entry = entries.first(where: { $0.id == id }), let report = entry.report else { return 0 }
        let originals = libraryFiles(of: id)
        guard originals.isEmpty, entry.state == .ready else { return originals.count }
        let version = paths.versionDirectory(for: entry)
        do {
            var archives: [String?: ZipArchive] = [:]
            for file in [entry.base].compactMap({ $0 }) + entry.splits {
                archives[file.splitName] = try ZipArchive(url: version.appendingPathComponent(file.fileName))
            }
            let installed = try installNativeCode(of: report, from: archives, into: version, signing: nil)
            // The findings are the current ones: those of an earlier check are replaced, never added to.
            try record(id: id) { entry in
                entry.report?.compatibility.issues.removeAll {
                    switch $0 {
                    case .libraryDegraded, .libraryCallsStubs: true
                    default: false
                    }
                }
                entry.report?.compatibility.issues += installed.findings
                entry.report?.compatibility.checks = installed.checks
            }
        } catch let refusal as RefusedLibrary {
            try refuse(id: id, refusal)
            throw ConversionRefused(reason: refusal.reason)
        }
        return libraryFiles(of: id).count
    }

    /// Marks the entry as incompatible because the converter refuses one of its libraries, as the import
    /// does: its version keeps neither its APKs nor the code (ADR-0032).
    func refuse(id: String, _ refusal: RefusedLibrary) throws {
        guard let entry = entries.first(where: { $0.id == id }) else { return }
        let splits = entry.splits // their files go below, once the record no longer names them
        try record(id: id) { entry in
            entry.state = .incompatible
            entry.report?.compatibility.status = .incompatible
            entry.report?.compatibility.issues += refusal.issues
            entry.base = nil
            entry.splits = []
            entry.signedWith = nil
        }
        let version = paths.versionDirectory(for: entry)
        try? FileManager.default.removeItem(at: version.appendingPathComponent("lib"))
        try? FileManager.default.removeItem(at: version.appendingPathComponent("base.apk"))
        for split in splits {
            try? FileManager.default.removeItem(at: version.appendingPathComponent(split.fileName))
        }
    }
}
