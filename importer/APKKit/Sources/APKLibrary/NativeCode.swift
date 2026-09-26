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

/// A library of the app that cannot run: refused by the converter, or damaged in the APK.
struct RefusedLibrary: Error {
    var library: String
    var reason: String

    init(library: String, reason: String) {
        self.library = library
        self.reason = reason
    }

    /// For an error reading the library out of the APK: damage, which no second try repairs, or our own
    /// limit; nil for an error of reading the file, which may not happen again.
    init?(readError: any Error, library: String) {
        switch readError as? APKError {
        case .truncated, .crcMismatch, .encryptedEntry, .unsupportedCompression:
            self.init(library: library, reason: "its data in the APK is damaged")
        case .entryTooLarge:
            self.init(library: library, reason: "it is larger than \(ZipArchive.defaultMaxEntrySize >> 20) MiB, which Minato cannot extract yet")
        default:
            return nil
        }
    }
}

extension Library {
    /// Extracts the app's arm64-v8a libraries, the ELF originals, into the version's lib/ and converts
    /// each into its container <name>.so.dylib (ADR-0008). Returns the certificate that signed them all;
    /// nil without one or when one could not be signed, and then only the originals stay, for the first
    /// tap to sign. Throws `RefusedLibrary` for a library that cannot run.
    func installNativeCode(of report: APKReport, from archive: ZipArchive, into versionDirectory: URL,
                           signing: ImportSigning?) throws -> String? {
        // Android's installer extracts only these and skips the others (NativeLibraryHelper).
        let libraries = report.nativeLibraries.filter { Self.isExtracted($0.name) }
        var names = Set<String>()
        for library in libraries where !names.insert(library.name).inserted {
            throw RefusedLibrary(library: library.name, reason: "it is in the APK twice")
        }

        let lib = versionDirectory.appendingPathComponent("lib", isDirectory: true)
        try FileManager.default.createDirectory(at: lib, withIntermediateDirectories: true)
        var originals: [URL] = []
        for library in libraries {
            let contents: Data
            do {
                guard let entry = archive.entry(named: library.path) else { throw APKError.entryNotFound(library.path) }
                contents = try archive.data(for: entry)
            } catch {
                if let refusal = RefusedLibrary(readError: error, library: library.name) { throw refusal }
                throw error
            }
            let original = lib.appendingPathComponent(library.name)
            try contents.write(to: original, options: .atomic)
            originals.append(original)
        }
        guard let signing else { return nil }

        // Every library is converted, also after one that could not be signed, so that none that cannot
        // run slips through.
        var signed = true
        for original in originals {
            do {
                try signing.converter.convert(original, to: original.appendingPathExtension("dylib"))
            } catch let refusal as ConversionRefused {
                throw RefusedLibrary(library: original.lastPathComponent, reason: refusal.reason)
            } catch {
                signed = false
            }
        }
        guard signed, let fingerprint = signing.fingerprint else {
            for original in originals {
                try? FileManager.default.removeItem(at: original.appendingPathExtension("dylib"))
            }
            return nil
        }
        return fingerprint
    }

    /// The names that Android's installer extracts from lib/<abi>/: lib*.so of A-Z, a-z, 0-9 and
    /// + , - . = _ (isFilenameSafe).
    static func isExtracted(_ name: String) -> Bool {
        name.hasPrefix("lib") && name.hasSuffix(".so")
            && name.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || "+,-.=_".contains($0)) }
    }

    /// Makes sure the originals of a ready NativeActivity app's native code are in its version's lib/,
    /// extracting them from its base.apk for an entry imported before the import did; the first tap signs
    /// them (ADR-0032, decision 3). Returns how many there are. A library that cannot run makes the app
    /// incompatible, as at import.
    @discardableResult
    public func prepareNativeCode(id: String) throws -> Int {
        guard let entry = entries.first(where: { $0.id == id }), let report = entry.report else { return 0 }
        let originals = libraryFiles(of: id)
        guard originals.isEmpty, entry.state == .ready, case .nativeActivity = report.runtime else { return originals.count }
        let version = paths.versionDirectory(for: entry)
        do {
            _ = try installNativeCode(of: report, from: ZipArchive(url: version.appendingPathComponent("base.apk")),
                                      into: version, signing: nil)
        } catch let refusal as RefusedLibrary {
            try refuse(id: id, library: refusal.library, reason: refusal.reason)
            throw ConversionRefused(reason: refusal.reason)
        }
        return libraryFiles(of: id).count
    }

    /// Marks the entry as incompatible because the converter refuses one of its libraries, as the import
    /// does: its version keeps neither the APK nor the code (ADR-0032).
    func refuse(id: String, library: String, reason: String) throws {
        guard let entry = entries.first(where: { $0.id == id }) else { return }
        try record(id: id) { entry in
            entry.state = .incompatible
            entry.report?.compatibility.status = .incompatible
            entry.report?.compatibility.issues.append(.conversionFailed(library: library, reason: reason))
            entry.base = nil
            entry.signedWith = nil
        }
        let version = paths.versionDirectory(for: entry)
        try? FileManager.default.removeItem(at: version.appendingPathComponent("lib"))
        try? FileManager.default.removeItem(at: version.appendingPathComponent("base.apk"))
    }
}
