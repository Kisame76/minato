import APKRuntime
import SwiftUI
import os

@main
struct MinatoApp: App {
    @State private var library: LibraryModel
    @State private var signing: SigningCoordinator
    @Environment(\.scenePhase) private var scenePhase

    init() {
        let library = LibraryModel()
        let signing = SigningCoordinator(library: library)
        // The import converts and signs native code while the certificate is valid (ADR-0032).
        library.importSigning = { [weak signing] in signing?.importSigning() }
        _library = State(initialValue: library)
        _signing = State(initialValue: signing)
        Logger(subsystem: "io.github.kisame76.minato", category: "runtime")
            .info("Runtime \(String(cString: apkrt_version()), privacy: .public)")
        #if DEBUG
        SpikeRunner.runIfRequested()
        FixtureRunner.runIfRequested()
        CertificateHandover.runIfRequested()
        CertificateCheck.runIfRequested()
        #endif
    }

    var body: some Scene {
        WindowGroup {
            LibraryView()
                .environment(library)
                .environment(signing)
                .onOpenURL { url in
                    // The stores' certificate export answers with minato://; anything else is a file.
                    if !signing.handle(url) {
                        Task { await library.open(url) }
                    }
                }
                .sheet(isPresented: $signing.showsSetup) {
                    CertificateSetupView()
                        .environment(signing)
                }
        }
        // Finish imports that were interrupted, e.g. because the app was closed while installing, and
        // check the signing certificate, which a store may have changed while the app was away.
        .onChange(of: scenePhase, initial: true) { _, phase in
            if phase == .active {
                Task { await library.refresh() }
                Task { await signing.refresh() }
            }
        }
    }
}
