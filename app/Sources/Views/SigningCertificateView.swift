import APKSigning
import SwiftUI
import UniformTypeIdentifiers

/// Shows the development certificate APK-Runner signs converted code with (ADR-0022, ADR-0023).
/// APK-Runner gets it from SideStore or AltStore by itself; a .p12 file can still be imported by
/// hand, which the app checks the same way before keeping it. The diagnostics are one level down.
struct SigningCertificateView: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(SigningCoordinator.self) private var signing
    @State private var isPicking = false
    @State private var pickedFile: PickedFile?
    @State private var password = ""
    @State private var isWorking = false
    @State private var errorMessage: String?
    @State private var confirmsRemoval = false

    private struct PickedFile: Identifiable {
        let id = UUID()
        let name: String
        let data: Data
    }

    var body: some View {
        let certificate = CodeSigning.storedCertificateInfo()
        @Bindable var signing = signing
        NavigationStack {
            Form {
                Section {
                    LabeledContent("Status", value: status)
                    if let certificate {
                        LabeledContent("Name", value: certificate.commonName)
                        LabeledContent("Team", value: certificate.team)
                        LabeledContent("Expires", value: certificate.notAfter.formatted(date: .abbreviated, time: .omitted))
                    }
                } footer: {
                    Text("APK-Runner signs the native code of your apps with your development certificate, so that iOS runs it. It gets the certificate from SideStore or AltStore by itself; you can also import a .p12 file. The certificate stays in the keychain of this iPhone.")
                }
                Section {
                    Button(certificate == nil ? "Import Certificate…" : "Replace Certificate…") { isPicking = true }
                        .disabled(isWorking)
                    if certificate != nil {
                        Button("Remove Certificate", role: .destructive) { confirmsRemoval = true }
                            .disabled(isWorking)
                    }
                }
                Section {
                    NavigationLink("Diagnostics") { SigningDiagnosticsView() }
                }
            }
            .navigationTitle("Signing")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .overlay {
                if isWorking {
                    ProgressView("Checking the certificate…")
                        .padding()
                        .background(.regularMaterial, in: .rect(cornerRadius: 12))
                }
            }
            .fileImporter(isPresented: $isPicking, allowedContentTypes: [.pkcs12]) { result in
                guard case .success(let url) = result else { return }
                read(url)
            }
            .alert("Certificate Password", isPresented: passwordShown, presenting: pickedFile) { file in
                SecureField("Password", text: $password)
                Button("Import") { importCertificate(file) }
                Button("Cancel", role: .cancel) { password = "" }
            } message: { file in
                Text("Enter the password you chose when you exported “\(file.name)”.")
            }
            .alert("Certificate Not Imported", isPresented: errorShown) {
                Button("OK") { errorMessage = nil }
            } message: {
                Text(errorMessage ?? "")
            }
            // Over this sheet, the setup sheet can only come from here, e.g. after Remove or a
            // simulated change in the diagnostics.
            .sheet(isPresented: $signing.showsSetup) {
                CertificateSetupView()
            }
            .confirmationDialog("Remove the certificate?", isPresented: $confirmsRemoval, titleVisibility: .visible) {
                Button("Remove Certificate", role: .destructive) {
                    Task { await signing.removeCertificate() }
                }
            } message: {
                Text("Apps whose code is already signed keep working until the certificate expires. APK-Runner gets it again from SideStore or AltStore when it can.")
            }
        }
    }

    private var status: String {
        switch signing.activity {
        case .importing: return "Checking…"
        case .waitingForStore(let store): return "Waiting for \(store.name)…"
        case .resigning: return "Updating apps…"
        case .idle: break
        }
        switch signing.state {
        case .valid: return "Ready"
        case .notNeeded: return "Not needed in the Simulator"
        case .ownCertificateUnreadable: return "Can’t read the app’s own certificate"
        case .missing: return "No certificate"
        case .changed: return "Changed, getting the new one"
        case .expired: return "Expired"
        case .selfTestFailed: return "iOS rejects code signed with it"
        }
    }

    private func read(_ url: URL) {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }
        do {
            pickedFile = PickedFile(name: url.lastPathComponent, data: try Data(contentsOf: url))
        } catch {
            errorMessage = "The file could not be read: \(error.localizedDescription)"
        }
    }

    private func importCertificate(_ file: PickedFile) {
        let entered = password
        password = ""
        isWorking = true
        Task {
            errorMessage = await signing.importFile(file.data, password: entered)
            isWorking = false
        }
    }

    private var passwordShown: Binding<Bool> {
        Binding(get: { pickedFile != nil }, set: { if !$0 { pickedFile = nil } })
    }

    private var errorShown: Binding<Bool> {
        Binding(get: { errorMessage != nil }, set: { if !$0 { errorMessage = nil } })
    }
}
