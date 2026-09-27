import APKKit
import APKLibrary
import SwiftUI

struct AppDetailView: View {
    let entry: LibraryEntry
    @Environment(LibraryModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var confirmsRemoval = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack(spacing: 16) {
                        AppIcon(url: iconURL)
                            .frame(width: 64, height: 64)
                        VStack(alignment: .leading, spacing: 4) {
                            Text(entry.displayName).font(.headline)
                            StatusView(status: Texts.status(of: entry))
                        }
                    }
                }

                if let report = entry.report {
                    if !report.compatibility.issues.isEmpty {
                        Section("Compatibility") {
                            ForEach(Array(report.compatibility.issues.enumerated()), id: \.offset) { _, issue in
                                Label(Texts.issue(issue), systemImage: Texts.isBlocking(issue) ? "xmark.octagon" : "info.circle")
                            }
                        }
                    }
                    Section("App") {
                        LabeledContent("Package", value: report.packageName)
                        LabeledContent("Version", value: "\(report.versionName ?? "–") (\(report.versionCode))")
                        LabeledContent("Android", value: "min API \(report.minSdk), target API \(report.targetSdk)")
                        LabeledContent("Launch Activity", value: report.launcherActivity ?? "–")
                    }
                    Section("Technical") {
                        LabeledContent("Runtime", value: Texts.runtime(report.runtime))
                        LabeledContent("Engine", value: report.engine ?? "–")
                        LabeledContent("ABIs", value: report.abis.isEmpty ? "none (no native code)" : report.abis.joined(separator: ", "))
                        if !report.nativeLibraries.isEmpty {
                            LabeledContent("Native Libraries (arm64)", value: "\(report.nativeLibraries.count)")
                        }
                        LabeledContent("OpenGL ES", value: report.requiredGLESVersion.map(Texts.glesVersion) ?? "–")
                        LabeledContent("Vulkan", value: report.requiredVulkanVersion.map(Texts.vulkanVersion) ?? "–")
                    }
                    if !report.playDependencies.isEmpty {
                        Section("Google Play") {
                            ForEach(report.playDependencies, id: \.self) { Text(Texts.playDependency($0)) }
                        }
                    }
                    let signer = report.signature.signerDetails
                    Section {
                        LabeledContent("Schemes", value: report.signature.schemes.map(\.rawValue).joined(separator: ", ").nonEmpty ?? "unsigned")
                        LabeledContent("Check", value: Texts.signatureCheck(report.signature.check))
                        ForEach(signer.certificates, id: \.sha256) { certificate in
                            CertificateRow(role: "Signer", certificate: certificate, isProven: signer.isProven)
                        }
                        ForEach(signer.earlier, id: \.sha256) { certificate in
                            CertificateRow(role: "Earlier key", certificate: certificate, isProven: signer.isProven)
                        }
                        if report.signature.hasGooglePlayStamp {
                            Label("Google Play source stamp, not verified", systemImage: "seal")
                                .foregroundStyle(.secondary)
                        }
                    } header: {
                        Text("Signature")
                    } footer: {
                        if !signer.isProven, !signer.certificates.isEmpty {
                            Text("The check did not prove these certificates; the file only names them.")
                        }
                    }
                }

                Section("File") {
                    LabeledContent("Source", value: entry.source.fileName)
                    LabeledContent("Size", value: ByteCountFormatter.string(fromByteCount: Int64(entry.source.size), countStyle: .file))
                }

                Section {
                    Button("Delete", role: .destructive) { confirmsRemoval = true }
                }
            }
            .navigationTitle(entry.displayName)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .confirmationDialog("Delete “\(entry.displayName)”?", isPresented: $confirmsRemoval, titleVisibility: .visible) {
                Button("Delete", role: .destructive) {
                    Task {
                        await model.remove([entry])
                        dismiss()
                    }
                }
            } message: {
                Text("Its installed files and app data will be deleted. The original APK is kept.")
            }
        }
    }

    private var iconURL: URL? {
        entry.iconFile.flatMap { file in
            (try? LibraryPaths.standard()).map { $0.applicationSupport.appendingPathComponent(file) }
        }
    }
}

/// A certificate of the signer, marked when the signature check did not prove it (ADR-0034, step d).
private struct CertificateRow: View {
    let role: String
    let certificate: SigningCertificate
    let isProven: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                Text(role)
                if !isProven {
                    Label("Not proven", systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.orange)
                }
            }
            .font(.caption)
            .foregroundStyle(.secondary)
            Text(certificate.subject ?? "Unknown")
            Text(certificate.sha256)
                .font(.caption2.monospaced())
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
        }
    }
}

private extension String {
    var nonEmpty: String? { isEmpty ? nil : self }
}
