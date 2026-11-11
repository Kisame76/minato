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
        // device-regressions entry 3's control (EdgeControl): a bare view controller in landscape, nothing else.
        if EdgeControl.isRequested {
            AppDelegate.orientations = .landscape
            mode = .library(nil)
            return
        }
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
            // Conflicts of the user's files a session left unasked, at its end or killed (ADR-0057's addendum).
            WriteBackHost.askLeftConflicts()
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
        #if DEBUG
        if EdgeControl.isRequested {
            EdgeControl.Placeholder()
        } else {
            chosenView
        }
        #else
        chosenView
        #endif
    }

    @ViewBuilder private var chosenView: some View {
        switch mode {
        case .gameMode(let entry):
            GameModeView(entry: entry)
                .onOpenURL { url in Self.takeOverDuringSession(url) }
        case .compileMode(let entry):
            CompileModeView(entry: entry)
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
        // A notification's checked content intent, which the app starts with (ADR-0074 decision 4).
        if case .gameMode = mode { GameSession.startIntent = request?.startIntent }
        // An end of game mode that went wrong shows once, at the library's next start without a request.
        if case .library(nil) = mode, let notice = LastSessionStore(folder: paths.applicationSupport).take() {
            mode = .library(.quitUnexpectedly(notice))
        }
        // A session that crashed or that iOS ended left its raw log: its last lines are kept now (ADR-0070).
        try? SessionLog(folder: paths.sessionLogDirectory).keepLeftovers()
        // An app installer's copy that a game mode ending halfway left in the temporary folder.
        InstallPresenter.removeLeftovers()
        if let request {
            let seconds = Date.now.timeIntervalSince(request.time)
            let chosen = switch mode {
            case .gameMode: "game mode"
            case .compileMode: "compile mode"
            case .library: "the library"
            }
            log.info("Launch request \(request.package, privacy: .private(mask: .hash)): \(chosen, privacy: .public)")
            log.info("The launch request came \(seconds, format: .fixed(precision: 3)) s after the tap")
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
                log.error(
                    "“\(url.lastPathComponent)” could not be taken over: \(error.localizedDescription)")
            }
        }
    }
}
