import APKSession
import SwiftUI

/// The library's process (ADR-0032, decision 1): the library with its signing certificate, and why a
/// launch request led here instead of game mode.
struct LibraryScene: View {
    let library: LibraryModel
    @Bindable var signing: SigningCoordinator
    @State private var problem: LaunchMode.Problem?
    @Environment(\.scenePhase) private var scenePhase

    init(library: LibraryModel, signing: SigningCoordinator, problem: LaunchMode.Problem?) {
        self.library = library
        self.signing = signing
        _problem = State(initialValue: problem)
    }

    var body: some View {
        LibraryView()
            .environment(library)
            .environment(signing)
            .onOpenURL { url in
                // The restart's own URL is nothing to do; the stores' certificate export answers with
                // minato:// too; anything else is a file.
                guard !Restart.isRestart(url) else { return }
                if !signing.handle(url) {
                    Task { await library.open(url) }
                }
            }
            .sheet(isPresented: $signing.showsSetup) {
                CertificateSetupView()
                    .environment(signing)
            }
            .alert(problem.map(Texts.launchProblem)?.title ?? "", isPresented: problemShown) {
                Button("OK") {}
            } message: {
                Text(problem.map(Texts.launchProblem)?.message ?? "")
            }
            // Finish imports that were interrupted, e.g. because the app was closed while installing, and
            // check the signing certificate, which a store may have changed while the app was away.
            .onChange(of: scenePhase, initial: true) { _, phase in
                if phase == .active {
                    Task {
                        await library.refresh()
                        #if DEBUG
                        await SessionCheck.tapIfRequested(in: library)
                        #endif
                    }
                    Task { await signing.refresh() }
                }
            }
    }

    private var problemShown: Binding<Bool> {
        Binding(get: { problem != nil }, set: { if !$0 { problem = nil } })
    }
}
