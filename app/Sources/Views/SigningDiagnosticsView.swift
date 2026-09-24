import APKSigning
import SwiftUI

/// The hidden diagnostics (ADR-0023), reached through More › Signing Certificate › Diagnostics:
/// everything the signing automation knows and did, and the actions to run parts of it by hand.
struct SigningDiagnosticsView: View {
    @Environment(SigningCoordinator.self) private var signing
    @State private var isWorking = false
    @State private var confirmsRemoval = false

    var body: some View {
        let diagnostics = SigningDiagnostics.current(signing)
        Form {
            ForEach(diagnostics.sections) { section in
                Section(section.title) {
                    ForEach(section.rows) { row in
                        LabeledContent(row.label) {
                            Text(row.value)
                                .textSelection(.enabled)
                                .multilineTextAlignment(.trailing)
                        }
                    }
                }
            }
            Section {
                Button("Run Self-Test") { run { await signing.runSelfTest() } }
                if case .store(let store) = signing.environment.installer {
                    Button("Get Certificate from \(store.name) Again") { run { await signing.getCertificate(from: store) } }
                }
                Button("Simulate Certificate Change") { run { await signing.simulateChange() } }
                Button("Remove Certificate", role: .destructive) { confirmsRemoval = true }
            } footer: {
                Text("Simulate Certificate Change treats the stored certificate as an old one: APK-Runner gets it again and signs your apps again, as after a real change.")
            }
            .disabled(isWorking)
        }
        .navigationTitle("Diagnostics")
        .navigationBarTitleDisplayMode(.inline)
        .confirmationDialog("Remove the certificate?", isPresented: $confirmsRemoval, titleVisibility: .visible) {
            Button("Remove Certificate", role: .destructive) { run { await signing.removeCertificate() } }
        } message: {
            Text("APK-Runner gets it again from SideStore or AltStore when it can.")
        }
    }

    private func run(_ action: @escaping @MainActor () async -> Void) {
        isWorking = true
        Task {
            await action()
            isWorking = false
        }
    }
}
