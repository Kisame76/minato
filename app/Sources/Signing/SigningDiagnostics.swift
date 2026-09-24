import APKSigning
import Foundation
import os

/// What the hidden diagnostics show about the signing certificate (ADR-0023). Every row has a
/// stable key, so that tools/certificate-check.sh reads the same values the screen shows.
struct SigningDiagnostics {
    struct Row: Identifiable {
        let key: String
        let label: String
        let value: String
        var id: String { key }
    }

    struct Section: Identifiable {
        let title: String
        let rows: [Row]
        var id: String { title }
    }

    let sections: [Section]

    var rows: [Row] { sections.flatMap(\.rows) }

    @MainActor
    static func current(_ coordinator: SigningCoordinator) -> SigningDiagnostics {
        let record = coordinator.records.load()
        let environment = coordinator.environment
        let imported = CodeSigning.storedCertificateInfo()
        let own = environment.own
        let reachable = environment.reachableStores()

        let match: String
        switch compare(imported: imported, own: own) {
        case .same: match = "yes"
        case .adHoc: match = "not needed (ad hoc)"
        case .none: match = "nothing imported"
        case .renewed, .otherTeam: match = "no"
        }
        let keychain = CodeSigning.storedIdentity() != nil ? "identity"
            : CodeSigning.storedPrivateKey() != nil ? "key and certificate" : "empty"

        return SigningDiagnostics(sections: [
            Section(title: "Status", rows: [
                Row(key: "state", label: "State", value: describe(coordinator.state)),
                Row(key: "next", label: "Next Step", value: describe(coordinator.next)),
                Row(key: "source", label: "Source", value: record.source?.rawValue ?? "none"),
                Row(key: "imported.at", label: "Imported", value: record.importedAt.map(day) ?? "never"),
                Row(key: "match", label: "Matches the App’s", value: match),
                Row(key: "keychain", label: "Keychain", value: keychain),
            ]),
            Section(title: "Imported Certificate", rows: certificateRows("imported", imported)),
            Section(title: "The App’s Certificate", rows: (environment.ownSignature.problem.map {
                [Row(key: "own.sha256", label: "Certificate", value: "unreadable: \($0)")]
            } ?? certificateRows("own", own)) + [Row(key: "own.chain", label: "Chain in Its Signature", value: CodeSigning.ownChain())]),
            Section(title: "Installation", rows: [
                Row(key: "installer", label: "Installed By", value: describe(environment.installer)),
                Row(key: "stores", label: "Stores Found",
                    value: reachable.isEmpty ? "none" : reachable.map(\.rawValue).sorted().joined(separator: ", ")),
                Row(key: "embedded", label: "ALTCertificate.p12",
                    value: environment.embeddedCertificate == nil ? "no" : "yes"),
            ]),
            Section(title: "Self-Test", rows: [
                Row(key: "selftest", label: "Result",
                    value: record.selfTest.map { $0.passed ? "passed" : "failed" } ?? "not run"),
                Row(key: "selftest.message", label: "Details", value: record.selfTest?.message ?? "–"),
                Row(key: "selftest.date", label: "Run", value: record.selfTest.map { day($0.date) } ?? "never"),
            ]),
            Section(title: "Signing Again", rows: [
                Row(key: "resign.last", label: "Last Time", value: record.lastResign.map(describe) ?? "never"),
            ]),
            Section(title: "Memory", rows: [
                Row(key: "memory.available", label: "Available Now", value: availableMemory()),
                Row(key: "memory.entitlement", label: "Increased Memory Limit", value: increasedMemoryLimit()),
            ]),
        ])
    }

    private static func certificateRows(_ prefix: String, _ certificate: CertificateInfo?) -> [Row] {
        guard let certificate else {
            return [Row(key: "\(prefix).sha256", label: "Certificate", value: prefix == "own" ? "none (signed ad hoc)" : "none")]
        }
        return [
            Row(key: "\(prefix).name", label: "Name", value: certificate.commonName),
            Row(key: "\(prefix).team", label: "Team", value: certificate.team),
            Row(key: "\(prefix).serial", label: "Serial Number", value: certificate.serialNumber),
            Row(key: "\(prefix).sha256", label: "SHA-256", value: certificate.sha256),
            Row(key: "\(prefix).expires", label: "Valid Until", value: day(certificate.notAfter)),
        ]
    }

    /// What iOS still lets the app use; the Simulator does not report it.
    private static func availableMemory() -> String {
        let available = os_proc_available_memory()
        return available == 0 ? "not reported" : ByteCountFormatter.string(fromByteCount: Int64(available), countStyle: .memory)
    }

    /// Whether the provisioning profile the app was installed with grants
    /// `increased-memory-limit` (ADR-0012): SideStore asks for it only for paid teams.
    private static func increasedMemoryLimit() -> String {
        guard let url = Bundle.main.url(forResource: "embedded", withExtension: "mobileprovision"),
              let data = try? Data(contentsOf: url)
        else { return "no profile" }
        guard let start = data.range(of: Data("<?xml".utf8)),
              let end = data.range(of: Data("</plist>".utf8), in: start.lowerBound..<data.endIndex),
              let plist = try? PropertyListSerialization.propertyList(from: data[start.lowerBound..<end.upperBound], format: nil)
                as? [String: Any]
        else { return "unreadable profile" }
        let entitlements = plist["Entitlements"] as? [String: Any]
        return entitlements?["com.apple.developer.kernel.increased-memory-limit"] as? Bool == true ? "yes" : "no"
    }

    private static func describe(_ state: SigningState) -> String {
        switch state {
        case .notNeeded: "notNeeded"
        case .ownCertificateUnreadable(let reason): "ownCertificateUnreadable: \(reason)"
        case .missing: "missing"
        case .valid: "valid"
        case .changed: "changed"
        case .expired: "expired"
        case .selfTestFailed(_, let reason): "selfTestFailed: \(reason)"
        }
    }

    private static func describe(_ step: NextStep) -> String {
        switch step {
        case .none: "none"
        case .importEmbedded: "importEmbedded"
        case .requestExport(let store, let automatically): "requestExport \(store.rawValue)\(automatically ? " (by itself)" : "")"
        case .offerFileImport: "offerFileImport"
        case .runSelfTest: "runSelfTest"
        case .resign(let count): "resign \(count)"
        }
    }

    private static func describe(_ installer: Installer) -> String {
        switch installer {
        case .store(let store): store.rawValue
        case .other: "other"
        }
    }

    private static func describe(_ resign: ResignRecord) -> String {
        "\(counted(resign.files, "file")) of \(counted(resign.entries, "app")), \(resign.failures.count) failed, \(day(resign.date))"
    }

    private static func counted(_ count: Int, _ noun: String) -> String {
        "\(count) \(noun)\(count == 1 ? "" : "s")"
    }

    private static func day(_ date: Date) -> String {
        date.formatted(.iso8601)
    }
}
