import APKLibrary
import APKRuntime
import APKSession
import SwiftUI
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

@main
struct MinatoApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    /// Game mode or the library, chosen once before anything else (ADR-0032, decision 1).
    private let mode: LaunchMode
    @State private var library: LibraryModel?
    @State private var signing: SigningCoordinator?

    init() {
        #if BENCHMARK
        // A benchmark run (ADR-0037) or a recorded session (ADR-0041 d) touches neither the launch request nor
        // the library's start.
        if BenchmarkMode.run != nil || BenchmarkMode.recordSession != nil {
            mode = .library(nil)
            return
        }
        #endif
        #if DEBUG
        // A fixture or ART test (FixtureRunner) touches neither the launch request nor the library: its program
        // starts once the launch has finished, in an empty scene.
        if FixtureRunner.startAfterLaunchIfRequested() {
            mode = .library(nil)
            return
        }
        #endif
        mode = Self.chooseMode()
        // Copies an earlier session left for Photos (ADR-0057 step e), before this one makes any.
        PhotoExports.removeEarlierCopies()
        // Before the first window, so that the library never turns.
        AppDelegate.orientations = UIInterfaceOrientationMask(mode.orientations)
        // Game mode creates no signing coordinator, scans no imports and shows no sheet.
        if case .library = mode {
            let library = LibraryModel()
            let signing = SigningCoordinator(library: library)
            // The import converts and signs native code while the certificate is valid, and a tap signs
            // what it did not (ADR-0032).
            library.importSigning = { [weak signing] in signing?.importSigning() }
            library.currentSigner = { [weak signing] in signing?.currentSigner() ?? .none }
            library.converter = { [weak signing] in signing?.converter(for: $0) }
            library.firstSigningLook = { [weak signing] in await signing?.firstLookEnded() }
            _library = State(initialValue: library)
            _signing = State(initialValue: signing)
        }
        Logger(subsystem: "io.github.kisame76.minato", category: "runtime")
            .info("Runtime \(String(cString: apkrt_version()), privacy: .public)")
        #if DEBUG
        SpikeRunner.runIfRequested()
        CertificateHandover.runIfRequested()
        CertificateCheck.runIfRequested()
        #endif
    }

    var body: some Scene {
        WindowGroup {
            #if BENCHMARK
            if let run = BenchmarkMode.run {
                BenchmarkView(run: run)
            } else if let package = BenchmarkMode.recordSession {
                SessionRecordingView(package: package)
            } else {
                modeView
            }
            #else
            modeView
            #endif
        }
    }

    @ViewBuilder private var modeView: some View {
        switch mode {
        case .gameMode(let entry):
            GameModeView(entry: entry)
                .onOpenURL { url in Self.takeOverDuringSession(url) }
        case .library(let problem):
            if let library, let signing {
                LibraryScene(library: library, signing: signing, problem: problem)
            }
        }
    }

    /// Reads and deletes the launch request, then looks for its app in the library's index.
    private static func chooseMode() -> LaunchMode {
        guard let paths = try? LibraryPaths.standard() else { return .library(nil) }
        let request = LaunchRequestStore(folder: paths.applicationSupport).take()
        var mode = LaunchMode(request: request, entries: (try? Library.entries(in: paths)) ?? [])
        // An end of game mode that went wrong shows once, at the library's next start without a request.
        if case .library(nil) = mode, let notice = LastSessionStore(folder: paths.applicationSupport).take() {
            mode = .library(.quitUnexpectedly(notice))
        }
        if let request {
            let seconds = Date.now.timeIntervalSince(request.time)
            let chosen = if case .gameMode = mode { "game mode" } else { "the library" }
            log.info("Launch request for \(request.package, privacy: .public), \(seconds, format: .fixed(precision: 3)) s after the tap: \(chosen, privacy: .public)")
        }
        #if DEBUG
        SessionCheck.processStarted(mode, request: request)
        #endif
        return mode
    }

    /// "Open with" during a session only takes the file into incoming/; the library installs it at its
    /// next launch (ADR-0018).
    private static func takeOverDuringSession(_ url: URL) {
        guard !Restart.isRestart(url) else { return }
        Task {
            let hasAccess = url.startAccessingSecurityScopedResource()
            defer {
                if hasAccess { url.stopAccessingSecurityScopedResource() }
            }
            do {
                _ = try await Library(paths: .standard()).adopt(url)
            } catch {
                log.error("“\(url.lastPathComponent, privacy: .public)” could not be taken over: \(error.localizedDescription, privacy: .public)")
            }
        }
    }
}
