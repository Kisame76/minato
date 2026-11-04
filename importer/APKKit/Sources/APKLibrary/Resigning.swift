import APKSigning
import Foundation

/// Turns an ELF file into a signed container. The app's ContainerSigner does this with the imported
/// certificate (ADR-0022); tests record what they were asked to do.
public protocol ContainerConverting: Sendable {
    func convert(_ elf: URL, to container: URL) throws
    /// The format version of the containers it writes (APK_CONTAINER_VERSION), which the entry records with the
    /// certificate, so that a container of an older format is converted again at the next tap.
    var containerVersion: UInt32 { get }
}

/// What `resignAll` did.
public struct ResignSummary: Sendable, Equatable {
    public var entries: Int
    public var files: Int
    /// "<entry>: <reason>" for each entry that could not be signed again.
    public var failures: [String]

    public init(entries: Int, files: Int, failures: [String]) {
        self.entries = entries
        self.files = files
        self.failures = failures
    }
}

extension Library {
    /// Every entry with the number of its converted libraries and the certificate they are signed
    /// with. Converted code lives in lib/: the ELF original <name>.so next to its container
    /// <name>.so.dylib (ADR-0008).
    public func resignCandidates() -> [ResignCandidate] {
        entries.map { entry in
            ResignCandidate(id: entry.id, signedWith: entry.signedWith, containerCount: libraryFiles(of: entry.id).count)
        }
    }

    /// Converts every library of the entry again from its ELF original, which also signs it with
    /// the converter's certificate, then records `fingerprint` as the one that signed it and the converter's format
    /// version of the containers. A failure
    /// leaves the old record, so the entry is tried again next time; a library the converter refuses
    /// makes the app incompatible, as at import (ADR-0032). Returns how many were converted. An app without native
    /// code counts as signed by it too, as at import, so that its next tap starts it (ADR-0039 c5).
    @discardableResult
    public func resign(id: String, fingerprint: String, with converter: some ContainerConverting) throws -> Int {
        let files = libraryFiles(of: id)
        for elf in files {
            do {
                try converter.convert(elf, to: elf.appendingPathExtension("dylib"))
            } catch let refusal as ConversionRefused {
                try refuse(id: id, RefusedLibrary(library: elf.lastPathComponent, reason: refusal.reason))
                throw refusal
            }
        }
        try record(id: id) {
            $0.signedWith = fingerprint
            $0.containerVersion = converter.containerVersion
        }
        return files.count
    }

    /// Signs again, entry by entry, all converted code that `fingerprint` did not sign
    /// (`entriesToResign`). `starting` hears of each entry before it runs, e.g. to show it as
    /// "Updating"; while it is awaited, the library serves other calls, such as an import. An entry
    /// that fails keeps its old record and is reported; the others go on.
    public func resignAll(fingerprint: String, with converter: some ContainerConverting,
                          starting: @Sendable (String) async -> Void = { _ in }) async -> ResignSummary {
        var summary = ResignSummary(entries: 0, files: 0, failures: [])
        for id in entriesToResign(resignCandidates(), current: fingerprint) {
            await starting(id)
            do {
                summary.files += try resign(id: id, fingerprint: fingerprint, with: converter)
                summary.entries += 1
            } catch {
                summary.failures.append("\(id): \(error.localizedDescription)")
            }
        }
        return summary
    }

    /// Forgets which certificate signed each entry, so that all converted code counts as signed by
    /// an old one: what "Simulate Certificate Change" needs to run the whole way (ADR-0023).
    public func forgetSignatures() throws {
        try recordAll { $0.signedWith = nil }
    }

    /// The ELF originals in the lib/ folder of the entry's installed version (ADR-0034), sorted by name; files that are no
    /// ELF files stay out.
    func libraryFiles(of id: String) -> [URL] {
        guard !id.hasPrefix("file:"), Self.isSafePackageName(id), let entry = entries.first(where: { $0.id == id }) else { return [] }
        let folder = paths.libraryDirectory(for: entry)
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        // A file the analysis found to be no ELF file (python-for-android's libpybundle.so) is not a library.
        let plain = Set(entry.report?.nativeLibraries.filter(\.isPlainFile).map(\.name) ?? [])
        return names.filter { $0.hasSuffix(".so") && !plain.contains($0) }.sorted().map { folder.appendingPathComponent($0) }
    }
}
