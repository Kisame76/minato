import APKLibrary
import APKSession
import SwiftUI

/// The library's process (ADR-0032, decision 1): the library with its signing certificate, and why a
/// launch request led here instead of game mode.
struct LibraryScene: View {
    let library: LibraryModel
    @Bindable var signing: SigningCoordinator
    @State private var problem: LaunchMode.Problem?
    @State private var reporting: AppReport?
    @State private var showsWelcome = LibraryScene.welcomeNeedsShowing
    @AppStorage(RootTab.storageKey) private var tab = RootTab.library
    @Environment(\.scenePhase) private var scenePhase

    init(library: LibraryModel, signing: SigningCoordinator, problem: LaunchMode.Problem?) {
        self.library = library
        self.signing = signing
        _problem = State(initialValue: problem)
    }

    var body: some View {
        RootView()
            .environment(library)
            .environment(signing)
            .onOpenURL { url in
                // The restart's own URL is nothing to do; the stores' certificate export answers with
                // minato:// too; anything else is a file.
                guard !Restart.isRestart(url) else { return }
                if !signing.handle(url) {
                    // "Open with" shows the file installing in Library (the app navigation's design, section 1).
                    tab = .library
                    Task { await library.open(url) }
                }
            }
            // The welcome first, then the certificate's setup, if it asks then too.
            .sheet(isPresented: $showsWelcome, onDismiss: { Welcome().shown() }) {
                WelcomeView()
            }
            .sheet(isPresented: setupShown) {
                CertificateSetupView()
                    .environment(signing)
            }
            .alert(problem.map(Texts.launchProblem)?.title ?? "", isPresented: problemShown, presenting: problem) {
                problem in
                if reportsApp(problem) {
                    Button("Report a Problem") { reporting = report(for: problem) }
                }
                Button("OK", role: .cancel) {}
            } message: { problem in
                Text(Texts.launchProblem(problem).message)
            }
            .sheet(item: $reporting) { report in
                ReportSheet(report: report)
            }
            #if DEBUG
            .preferredColorScheme(Self.checkAppearance)
            #endif
            // Finish imports that were interrupted, e.g. because the app was closed while installing, and
            // check the signing certificate, which a store may have changed while the app was away.
            .onChange(of: scenePhase, initial: true) { _, phase in
                if phase == .active {
                    Task {
                        await library.refresh()
                        #if DEBUG
                        await SessionCheck.tapIfRequested(in: library)
                        await SessionCheck.turnLibraryIfRequested()
                        await SessionCheck.endIfRequested(in: library)
                        await UpdateCheck.endIfRequested(in: library)
                        #endif
                    }
                    Task { await signing.refresh() }
                }
            }
    }

    #if DEBUG
    /// For TabSwipeUITests: MINATO_APPEARANCE=light or dark in the launch environment holds the library in that
    /// appearance whatever the device's is, so that a black page means no page also on an iPhone in dark mode.
    private static var checkAppearance: ColorScheme? {
        switch ProcessInfo.processInfo.environment["MINATO_APPEARANCE"] {
        case "light": .light
        case "dark": .dark
        default: nil
        }
    }
    #endif

    /// Whether the alert names an app to report (ADR-0070): one that is not ready, or that quit unexpectedly.
    private func reportsApp(_ problem: LaunchMode.Problem) -> Bool {
        switch problem {
        case .appNotFound: false
        case .appNotReady: true
        case .quitUnexpectedly(let notice): notice.package != nil
        }
    }

    /// The app's report, with how its last session ended when it quit unexpectedly.
    private func report(for problem: LaunchMode.Problem) -> AppReport? {
        switch problem {
        case .appNotFound:
            return nil
        case .appNotReady(let entry):
            return library.report(of: entry)
        case .quitUnexpectedly(let notice):
            guard let entry = library.items.first(where: { $0.id == notice.package })?.entry else { return nil }
            return library.report(of: entry, ending: AppReport.Ending(notice.cause), record: notice.record)
        }
    }

    /// Not while tools/session-check.sh or another check drives the library: the sheet would cover what it looks at.
    private static var welcomeNeedsShowing: Bool {
        #if DEBUG
        if FileManager.default.fileExists(atPath: SessionCheck.folder.path) { return false }
        #endif
        return Welcome().needsShowing
    }

    private var setupShown: Binding<Bool> {
        Binding(get: { signing.showsSetup && !showsWelcome }, set: { signing.showsSetup = $0 })
    }

    private var problemShown: Binding<Bool> {
        Binding(get: { problem != nil }, set: { if !$0 { problem = nil } })
    }
}
