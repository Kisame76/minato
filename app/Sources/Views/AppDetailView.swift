import APKKit
import APKLibrary
import SwiftUI

struct AppDetailView: View {
    let entry: LibraryEntry
    @Environment(LibraryModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var confirmsRemoval = false
    @State private var confirmsClearing = false
    @State private var profile: AppProfile?
    @State private var report: AppReport?
    /// The seconds after which the app closed itself at its last start.
    @State private var closedItself: Int?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack(spacing: 16) {
                        AppIcon(url: iconURL)
                            .frame(width: 64, height: 64)
                        VStack(alignment: .leading, spacing: 4) {
                            Text(entry.displayName).font(.headline)
                            StatusView(status: Texts.status(of: entry), alignment: .leading, showsReason: true)
                        }
                    }
                } footer: {
                    if let closedItself { Text(Texts.closedItself(entry.displayName, seconds: closedItself)) }
                }

                if entry.state == .ready, let shown = profile {
                    profileSection(shown)
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

                // A set's APKs (ADR-0048 b7b): what is installed, and what the choice for this iPhone left out.
                let skipped = entry.skippedSplits ?? []
                if !entry.splits.isEmpty || !skipped.isEmpty {
                    Section("Split APKs") {
                        if let base = entry.base {
                            LabeledContent("Base", value: size(base.size))
                        }
                        ForEach(entry.splits, id: \.fileName) { split in
                            LabeledContent(split.splitName ?? split.fileName, value: size(split.size))
                        }
                        ForEach(skipped, id: \.fileName) { split in
                            LabeledContent {
                                Text("Not installed")
                            } label: {
                                Text(split.splitName)
                                Text(split.reason)
                            }
                        }
                    }
                }

                Section("File") {
                    LabeledContent("Source", value: entry.source.fileName)
                    LabeledContent("Size", value: size(entry.source.size))
                }

                Section {
                    Button("Report a Problem") { report = model.report(of: entry) }
                }

                Section {
                    if !entry.id.hasPrefix("file:") {
                        Button("Clear App Data", role: .destructive) { confirmsClearing = true }
                    }
                    Button("Delete", role: .destructive) { confirmsRemoval = true }
                }
            }
            .navigationTitle(entry.displayName)
            .navigationBarTitleDisplayMode(.inline)
            .onAppear {
                profile = model.profile(of: entry)
                closedItself = model.closedItself(entry)
            }
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
            .clearDataDialog(isPresented: $confirmsClearing, entry: entry) { entry in
                Task { await model.clearData(of: entry) }
            }
            .sheet(item: $report) { report in
                ReportSheet(report: report)
            }
        }
    }

    /// The app's profile (ADR-0065 decision 6, ADR-0066 decision 7), kept at each change for the next start. 120 fps is
    /// offered where the screen shows it, or where it is the stored choice, which runs at the screen's highest rate.
    private func profileSection(_ shown: AppProfile) -> some View {
        let binding = Binding<AppProfile>(get: { shown }, set: { changed in
            profile = changed
            model.setProfile(changed, of: entry)
        })
        let screenMaximum = (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.screen
            .maximumFramesPerSecond ?? 60
        return Section {
            Picker("Device Profile", selection: binding.deviceProfile) {
                Text("Default").tag(AppProfile.DeviceProfile?.none)
                ForEach(AppProfile.DeviceProfile.allCases, id: \.self) { device in
                    Text(Texts.deviceProfile(device)).tag(Optional(device))
                }
            }
            Picker("Frame Rate", selection: binding.frameRate) {
                Text("As the App Asks").tag(AppProfile.FrameRate?.none)
                ForEach(AppProfile.FrameRate.allCases.filter { $0.rawValue <= screenMaximum || $0 == shown.frameRate },
                        id: \.self) { rate in
                    Text("\(rate.rawValue) fps").tag(Optional(rate))
                }
            }
        } header: {
            Text("Performance")
        } footer: {
            Text("Changes apply the next time the app opens. Apps see this phone's name and graphics chip.")
        }
    }

    private var iconURL: URL? {
        entry.iconFile.flatMap { file in
            (try? LibraryPaths.standard()).map { $0.applicationSupport.appendingPathComponent(file) }
        }
    }

    private func size(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
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
