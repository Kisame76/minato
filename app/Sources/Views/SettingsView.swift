import APKRuntime
import APKSigning
import SwiftUI
import UniformTypeIdentifiers

/// Settings (docs/plans/2026-09-24-app-navigation-design.md, section 4): a Form like the Settings app's, with the
/// signing certificate folded in (ADR-0023: normally one line, the actions only when a step by the user is needed,
/// everything technical in Diagnostics), the settings that work today, and About.
struct SettingsView: View {
    @Environment(SigningCoordinator.self) private var signing
    @AppStorage(GameSession.showsFPSKey) private var showsFPS = false

    var body: some View {
        let certificate = CodeSigning.storedCertificateInfo()
        NavigationStack {
            Form {
                Section {
                    LabeledContent {
                        Text(status)
                    } label: {
                        Text("Certificate")
                        if let certificate {
                            Text("Until \(certificate.notAfter.formatted(date: .abbreviated, time: .omitted))")
                        }
                    }
                    if signing.state.needsUser(next: signing.next) {
                        if case .store(let store) = signing.environment.installer {
                            Button("Get Certificate from \(store.name)") {
                                Task { await signing.getCertificate(from: store) }
                            }
                        }
                        CertificateFileButton()
                    }
                    NavigationLink("Diagnostics") { SigningDiagnosticsView() }
                } header: {
                    Text("Signing")
                } footer: {
                    Text("Minato gets your certificate from SideStore or AltStore by itself.")
                }
                Section {
                    Toggle("Show FPS", isOn: $showsFPS)
                } header: {
                    Text("Apps")
                } footer: {
                    Text("Shows the frame rate while an app runs. The app’s menu turns it on and off too.")
                }
                Section("About") {
                    LabeledContent("Version", value: version)
                    LabeledContent("Runtime", value: String(cString: apkrt_version()))
                    NavigationLink("Acknowledgements") { AcknowledgementsView() }
                }
            }
            .navigationTitle("Settings")
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

    /// MARKETING_VERSION (CURRENT_PROJECT_VERSION), from the Info.plist.
    private var version: String {
        let info = Bundle.main.infoDictionary
        let short = info?["CFBundleShortVersionString"] as? String ?? "?"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        return "\(short) (\(build))"
    }
}

/// "Import Certificate File…": a .p12 file and its password, which the app checks before keeping it; in Settings
/// when a step is needed and in Diagnostics always (section 4).
struct CertificateFileButton: View {
    @Environment(SigningCoordinator.self) private var signing
    @State private var isPicking = false
    @State private var pickedFile: PickedFile?
    @State private var password = ""
    @State private var isWorking = false
    @State private var errorMessage: String?

    private struct PickedFile: Identifiable {
        let id = UUID()
        let name: String
        let data: Data
    }

    var body: some View {
        Button("Import Certificate File…") { isPicking = true }
            .disabled(isWorking)
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

/// The licenses of the code inside the app (THIRD_PARTY.md), as tools/bundle-licenses.py puts them into the bundle's
/// Licenses/ with licenses.json as the index.
struct AcknowledgementsView: View {
    private struct Component: Decodable, Identifiable {
        let name: String
        let license: String
        let files: [String]

        var id: String { name }
    }

    private struct Index: Decodable {
        let components: [Component]
    }

    private let components: [Component] = {
        guard let url = Bundle.main.url(forResource: "licenses", withExtension: "json", subdirectory: "Licenses"),
              let data = try? Data(contentsOf: url) else { return [] }
        return (try? JSONDecoder().decode(Index.self, from: data).components) ?? []
    }()

    var body: some View {
        List(components) { component in
            NavigationLink {
                ScrollView {
                    Text(texts(of: component))
                        .font(.footnote.monospaced())
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding()
                }
                .navigationTitle(component.name)
                .navigationBarTitleDisplayMode(.inline)
            } label: {
                LabeledContent(component.name, value: component.license)
            }
        }
        .overlay {
            if components.isEmpty {
                ContentUnavailableView("No License Texts", systemImage: "doc.text")
            }
        }
        .navigationTitle("Acknowledgements")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func texts(of component: Component) -> String {
        component.files.compactMap { file in
            Bundle.main.url(forResource: file, withExtension: nil, subdirectory: "Licenses/\(component.name)")
                .flatMap { try? String(contentsOf: $0, encoding: .utf8) }
        }.joined(separator: "\n\n")
    }
}
