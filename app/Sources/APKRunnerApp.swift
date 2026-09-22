import SwiftUI

@main
struct APKRunnerApp: App {
    @State private var library = LibraryModel()
    @Environment(\.scenePhase) private var scenePhase

    init() {
        #if DEBUG
        SpikeRunner.runIfRequested()
        #endif
    }

    var body: some Scene {
        WindowGroup {
            LibraryView()
                .environment(library)
                .onOpenURL { url in
                    Task { await library.open(url) }
                }
        }
        // Finish imports that were interrupted, e.g. because the app was closed while installing.
        .onChange(of: scenePhase, initial: true) { _, phase in
            if phase == .active {
                Task { await library.refresh() }
            }
        }
    }
}
