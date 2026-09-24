import APKSigning
import SwiftUI
import UniformTypeIdentifiers

/// The one-time setup (ADR-0023). It shows only when the certificate has to come through a store's
/// export, which needs one tap in the store: the app opens the store by itself, and the store brings
/// it back. SideStore's embedded certificate and the deploy hand-over need no setup at all.
struct CertificateSetupView: View {
    @Environment(SigningCoordinator.self) private var signing
    @State private var isPicking = false
    @State private var pickedFile: PickedFile?
    @State private var password = ""
    @State private var fileError: String?

    private struct PickedFile: Identifiable {
        let id = UUID()
        let name: String
        let data: Data
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 24) {
                Image(systemName: "signature")
                    .font(.system(size: 48))
                    .foregroundStyle(.tint)
                Text(explanation)
                    .multilineTextAlignment(.center)
                status
                Spacer()
                if let store = signing.setupStore, !isBusy {
                    Button(signing.problem == nil ? "Get Certificate from \(store.name)" : "Try Again") {
                        Task { await signing.getCertificate(from: store) }
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                }
                Button("Import a .p12 File…") { isPicking = true }
                    .disabled(isBusy)
            }
            .padding(24)
            .navigationTitle("One-Time Setup")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Not Now") { signing.dismissSetup() }
                }
            }
            .fileImporter(isPresented: $isPicking, allowedContentTypes: [.pkcs12]) { result in
                guard case .success(let url) = result else { return }
                read(url)
            }
            .alert("Certificate Password", isPresented: passwordShown, presenting: pickedFile) { file in
                SecureField("Password", text: $password)
                Button("Import") { importFile(file) }
                Button("Cancel", role: .cancel) { password = "" }
            } message: { file in
                Text("Enter the password you chose when you exported “\(file.name)”.")
            }
            .alert("Certificate Not Imported", isPresented: fileErrorShown) {
                Button("OK") { fileError = nil }
            } message: {
                Text(fileError ?? "")
            }
        }
    }

    private var explanation: String {
        let changed = if case .changed = signing.state { "Your signing certificate has changed. " } else { "" }
        switch signing.setupStore {
        case .sideStore:
            return changed + "Minato needs your signing certificate to run apps. SideStore opens by itself: tap Export there, and it brings you back here."
        case .altStore:
            return changed + "Minato needs your signing certificate to run apps. AltStore opens by itself: tap Export there and leave the password empty."
        case nil:
            return changed + "Minato needs your signing certificate to run apps."
        }
    }

    @ViewBuilder
    private var status: some View {
        switch signing.activity {
        case .waitingForStore(let store):
            ProgressView("Waiting for \(store.name)…")
        case .importing:
            ProgressView("Checking the certificate…")
        case .resigning, .idle:
            if let problem = signing.problem {
                Text(message(for: problem))
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
        }
    }

    private func message(for problem: SigningCoordinator.Problem) -> String {
        switch problem {
        case .noCallback(let store):
            "No certificate received. This needs \(store.name) \(store.minimumVersion) or later. Update it, then try again."
        case .storeMissing(let store):
            "\(store.name) isn’t installed. Import a .p12 file instead."
        case .importFailed(let message):
            message
        }
    }

    private var isBusy: Bool {
        switch signing.activity {
        case .idle: false
        case .importing, .waitingForStore, .resigning: true
        }
    }

    private func read(_ url: URL) {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }
        do {
            pickedFile = PickedFile(name: url.lastPathComponent, data: try Data(contentsOf: url))
        } catch {
            fileError = "The file could not be read: \(error.localizedDescription)"
        }
    }

    private func importFile(_ file: PickedFile) {
        let entered = password
        password = ""
        Task { fileError = await signing.importFile(file.data, password: entered) }
    }

    private var passwordShown: Binding<Bool> {
        Binding(get: { pickedFile != nil }, set: { if !$0 { pickedFile = nil } })
    }

    private var fileErrorShown: Binding<Bool> {
        Binding(get: { fileError != nil }, set: { if !$0 { fileError = nil } })
    }
}
