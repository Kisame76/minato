import APKRuntime
import SwiftUI
import os

@main
struct APKRunnerApp: App {
    @State private var library = LibraryModel()
    @Environment(\.scenePhase) private var scenePhase

    init() {
        Logger(subsystem: "io.github.kisame76.apkrunner", category: "runtime")
            .info("Runtime \(String(cString: apkrt_version()), privacy: .public)")
        #if DEBUG
        SpikeRunner.runIfRequested()
        FixtureRunner.runIfRequested()
        CertificateHandover.runIfRequested()
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
