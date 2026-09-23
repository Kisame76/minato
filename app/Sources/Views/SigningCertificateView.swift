import SwiftUI
import UniformTypeIdentifiers

/// Shows the development certificate APK-Runner signs converted code with, and imports or removes
/// it (ADR-0022). Import: pick the .p12 file, enter its password; on a device the app then checks
/// that iOS accepts code signed with it before keeping it.
struct SigningCertificateView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var certificate = CodeSigning.storedCertificate()
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
        NavigationStack {
            Form {
                Section {
                    if let certificate {
                        LabeledContent("Name", value: certificate.name)
                        LabeledContent("Team", value: certificate.team)
                        LabeledContent("Expires", value: certificate.expires.formatted(date: .abbreviated, time: .omitted))
                    } else {
                        Text("No certificate imported")
                            .foregroundStyle(.secondary)
                    }
                } footer: {
                    Text("APK-Runner signs the native code of your apps with your development certificate, so that iOS runs it. Export the certificate as a .p12 file, for example in Xcode under Settings › Accounts › Manage Certificates, and import it here. It stays in the keychain of this iPhone.")
                }
                Section {
                    Button(certificate == nil ? "Import Certificate…" : "Replace Certificate…") { isPicking = true }
                        .disabled(isWorking)
                    if certificate != nil {
                        Button("Remove Certificate", role: .destructive) { confirmsRemoval = true }
                            .disabled(isWorking)
                    }
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
            .confirmationDialog("Remove the certificate?", isPresented: $confirmsRemoval, titleVisibility: .visible) {
                Button("Remove Certificate", role: .destructive) {
                    CodeSigning.removeIdentity()
                    certificate = nil
                }
            } message: {
                Text("Apps whose code is already signed keep working until the certificate expires. New apps cannot be prepared until you import a certificate again.")
            }
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
        Task.detached {
            let result = Result { try CodeSigning.importIdentity(from: file.data, password: entered) }
            await MainActor.run {
                isWorking = false
                switch result {
                case .success(let imported): certificate = imported
                case .failure(let error): errorMessage = error.localizedDescription
                }
            }
        }
    }

    private var passwordShown: Binding<Bool> {
        Binding(get: { pickedFile != nil }, set: { if !$0 { pickedFile = nil } })
    }

    private var errorShown: Binding<Bool> {
        Binding(get: { errorMessage != nil }, set: { if !$0 { errorMessage = nil } })
    }
}
