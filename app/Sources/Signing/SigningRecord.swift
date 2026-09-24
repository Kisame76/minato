import APKSigning
import Foundation

/// What APK-Runner remembers about its signing certificate besides the keychain items, none of it
/// secret (ADR-0023): Library/Application Support/signing.json.
struct SigningRecord: Codable, Equatable {
    var source: CertificateSource?
    var importedAt: Date?
    /// The fingerprint of the imported certificate.
    var sha256: String?
    var selfTest: SelfTestRecord?
    /// "Simulate Certificate Change" in the diagnostics.
    var markedStale = false
    /// The export link opened last, whose callback is still accepted.
    var pendingExport: ExportRequest?
    var lastResign: ResignRecord?
}

struct ResignRecord: Codable, Equatable {
    var date: Date
    var entries: Int
    var files: Int
    /// "<entry>: <reason>" for each entry that could not be signed again.
    var failures: [String]
}

struct SigningRecordFile: Sendable {
    let url: URL

    static var standard: SigningRecordFile {
        let folder = (try? FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                                   appropriateFor: nil, create: true))
            ?? URL.applicationSupportDirectory
        return SigningRecordFile(url: folder.appending(path: "signing.json"))
    }

    /// The record, or an empty one when there is none or it cannot be read: then the state machine
    /// runs the self-test again, which rebuilds what matters.
    func load() -> SigningRecord {
        guard let data = try? Data(contentsOf: url) else { return SigningRecord() }
        return (try? Self.decoder.decode(SigningRecord.self, from: data)) ?? SigningRecord()
    }

    func update(_ change: (inout SigningRecord) -> Void) throws {
        var record = load()
        change(&record)
        try Self.encoder.encode(record).write(to: url, options: .atomic)
    }

    private static let encoder: JSONEncoder = {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return encoder
    }()

    private static let decoder: JSONDecoder = {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return decoder
    }()
}
