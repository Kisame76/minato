import APKSigning
import Foundation

/// Turns an ELF file into a signed container. The app's ContainerSigner does this with the imported
/// certificate (ADR-0022); tests record what they were asked to do.
public protocol ContainerConverting: Sendable {
    func convert(_ elf: URL, to container: URL) throws
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
    /// the converter's certificate, then records `fingerprint` as the one that signed it. A failure
    /// leaves the old record, so the entry is tried again next time. Returns how many were converted.
    @discardableResult
    public func resign(id: String, fingerprint: String, with converter: some ContainerConverting) throws -> Int {
        let files = libraryFiles(of: id)
        guard !files.isEmpty else { return 0 }
        for elf in files {
            try converter.convert(elf, to: elf.appendingPathExtension("dylib"))
        }
        try record(id: id) { $0.signedWith = fingerprint }
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

    /// The ELF originals in the entry's lib/ folder, sorted by name.
    private func libraryFiles(of id: String) -> [URL] {
        guard !id.hasPrefix("file:"), Self.isSafePackageName(id) else { return [] }
        let folder = paths.libraryDirectory(for: id)
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        return names.filter { $0.hasSuffix(".so") }.sorted().map { folder.appendingPathComponent($0) }
    }
}
