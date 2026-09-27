import APKLibrary
import APKSession
import Foundation
import Observation
import SwiftUI
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

/// One app's session in game mode (ADR-0032): its lifecycle on ADR-0028's session, the menu, and the
/// way back to the library. The guest itself starts with ADR-0028's activity-host (its step d); until
/// then the session has no guest, the callbacks are logged and every wait for an answer ends at once.
@MainActor
@Observable
final class GameSession {
    let entry: LibraryEntry
    private(set) var menuIsOpen = false
    private(set) var showsHint: Bool
    /// The restart back to the library failed: the user opens Minato again.
    var restartFailed = false

    /// The window's size in pixels at its last layout.
    @ObservationIgnored private(set) var windowPixels = CGSize.zero
    /// The orientation iOS was asked to hold the window in.
    @ObservationIgnored private var heldOrientation: SessionOrientation?

    @ObservationIgnored private var bridge: SessionBridge
    @ObservationIgnored private let host: SessionHost
    /// `apkrt_session_wait` or its stand-in, run off the main thread.
    @ObservationIgnored private let waitForAnswer: @Sendable (UInt32, Double) -> Int32
    @ObservationIgnored private var started = false
    /// Why activity-host could not start the app, as it reported it.
    @ObservationIgnored private(set) var startFailure: String?

    @ObservationIgnored private var backgroundObserver: (any NSObjectProtocol)?

    /// The guest's touches (ADR-0036, decision 3), and where they go.
    @ObservationIgnored private var touches = TouchTranslator(scale: 1)
    @ObservationIgnored private let motionSink: MotionEventSink
    /// Ticks while the gate is open (ADR-0036, decision 2).
    @ObservationIgnored private let frameClock = DisplayLinkClock(sink: NoChannelFrameSink())
    /// The guest's audio output (ADR-0036, decision 4).
    @ObservationIgnored let audio = AudioHost(sink: NoChannelAudioSink())

    /// The session of this process, for the runtime's request handler.
    private(set) weak static var current: GameSession?

    nonisolated static let hintShownKey = "sessionMenuHintShown"

    init(entry: LibraryEntry) {
        self.entry = entry
        // Game mode starts only ready apps, which have a report; activity-host reports API level 35.
        bridge = SessionBridge(lifecycle: Lifecycle(targetSdk: entry.report?.targetSdk ?? 35))
        host = NoGuestSessionHost()
        waitForAnswer = { NoGuestSessionHost.wait(serial: $0, seconds: $1) }
        motionSink = NoChannelMotionSink()
        showsHint = !UserDefaults.standard.bool(forKey: Self.hintShownKey)
        Self.current = self
        SessionEndReport.begin(entry)
        // The GPU work must be scheduled before the background (ADR-0032, decision 6): the notification
        // comes on the main thread as the scene enters it, and SwiftUI's scene phase only later.
        backgroundObserver = NotificationCenter.default.addObserver(
            forName: UIScene.didEnterBackgroundNotification, object: nil, queue: nil
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.enteredBackground() }
        }
    }

    /// The orientation the session is to hold (ADR-0032, decision 5).
    var orientation: SessionOrientation {
        #if DEBUG
        if let requested = SessionCheck.requestedOrientation { return requested }
        #endif
        return SessionOrientation(screenOrientation: entry.report?.screenOrientation)
    }

    /// iOS was asked to hold the window in `orientation`, which turns it after a while.
    func holds(_ orientation: SessionOrientation) {
        heldOrientation = orientation
        startIfLaidOut()
        // iOS turns the window within a fraction of a second; should it never do so, nor refuse, the
        // guest still starts, with the size the window has.
        Task {
            try? await Task.sleep(for: .seconds(2))
            guard !started else { return }
            heldOrientation = .atStart
            startIfLaidOut()
        }
    }

    /// The window was laid out.
    func windowLaidOut(_ pixels: CGSize) {
        windowPixels = pixels
        // A window's size is fixed per ID (ADR-0028, c): the next window takes this one.
        bridge.windowSize = PixelSize(width: Int32(pixels.width.rounded()), height: Int32(pixels.height.rounded()))
        startIfLaidOut()
    }

    /// The guest starts with its window's final size, which ADR-0028 hands over with the window: at the
    /// first layout in the orientation the session holds.
    private func startIfLaidOut() {
        guard let heldOrientation, heldOrientation.fits(width: windowPixels.width, height: windowPixels.height) else { return }
        start()
    }

    /// The window is there; the guest starts with it, or with the foreground (Lifecycle).
    private func start() {
        guard !started else { return }
        started = true
        deliver(.start)
    }

    /// The guest started: the first session shows how to reach the menu for 3 s.
    private func guestStarted() {
        #if DEBUG
        SessionCheck.event(showsHint ? "hint shown" : "hint not shown")
        SessionCheck.gameModeStarted(self)
        #endif
        guard showsHint else { return }
        UserDefaults.standard.set(true, forKey: Self.hintShownKey)
        Task {
            try? await Task.sleep(for: .seconds(3))
            showsHint = false
        }
    }

    /// iOS moved the scene from `old` to `new`.
    func sceneChanged(from old: ScenePhase, to new: ScenePhase) {
        switch new {
        case .active:
            if old == .background { deliver(.foreground) }
            deliver(.active)
        case .inactive:
            deliver(old == .background ? .foreground : .inactive)
        case .background:
            if old == .active { deliver(.inactive) }
            deliver(.background)
        @unknown default:
            break
        }
    }

    /// iOS moved the scene to the background; SwiftUI's scene phase follows later, which then changes
    /// nothing.
    private func enteredBackground() {
        deliver(.inactive)
        deliver(.background)
    }

    func memoryWarning() {
        deliver(.memoryWarning)
    }

    func openMenu() {
        guard !menuIsOpen else { return }
        menuIsOpen = true
        #if DEBUG
        SessionCheck.event("menu opened")
        #endif
        deliver(.menuOpened)
    }

    func closeMenu() {
        guard menuIsOpen else { return }
        menuIsOpen = false
        #if DEBUG
        SessionCheck.event("menu closed")
        #endif
        deliver(.menuClosed)
    }

    /// Quit in the menu: the guest ends, then Minato restarts into the library.
    func quit() {
        menuIsOpen = false
        #if DEBUG
        SessionCheck.event("quit")
        #endif
        deliver(.quit)
    }

    /// A touch callback of the window, with its pixels per point.
    func touched(_ phase: TouchPhase, _ changes: [TouchChange], scale: Double) {
        #if DEBUG
        if phase == .began { changes.forEach { SessionCheck.touchClock($0.samples.last?.timestamp ?? 0) } }
        #endif
        touches.scale = scale
        post(touches.translate(phase, changes))
    }

    private func post(_ events: [MotionEvent]) {
        guard !events.isEmpty else { return }
        motionSink.post(events)
    }

    /// A request of the guest (ADR-0028, decision 4), on the main thread.
    func received(_ request: SessionRequest) {
        switch request {
        case .finish:
            deliver(.finish)
        case .keepScreenOn(let on):
            UIApplication.shared.isIdleTimerDisabled = on
        case .startFailed(let reason):
            log.error("\(self.entry.id, privacy: .public) cannot start: \(reason, privacy: .public)")
            startFailure = reason
            SessionEndReport.startFailed(reason)
        }
    }

    private func deliver(_ event: Lifecycle.Event) {
        report(bridge.handle(event, host: host))
    }

    /// Logs what the guest was sent, runs the wait for its answer, and returns to the library once the
    /// guest has ended or its channel broke.
    private func report(_ handled: SessionBridge.Handled) {
        for callback in handled.callbacks {
            log.info("\(self.entry.id, privacy: .public): \(String(describing: callback), privacy: .public)")
            #if DEBUG
            SessionCheck.event("callback \(callback)")
            #endif
        }
        if handled.callbacks.contains(.onCreate) { guestStarted() }
        frameClock.run(bridge.lifecycle.gate == .open && !bridge.isBroken)
        // A gesture that is open when the focus goes ends with a CANCEL (ADR-0036, decision 3).
        if handled.callbacks.contains(.onWindowFocusChanged(false)) {
            post(touches.focusLost(at: Double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) / 1e9))
        }
        if let wait = handled.wait { waitForGuest(wait) }
        if bridge.isBroken {
            log.error("\(self.entry.id, privacy: .public): the session's channel broke")
        }
        if bridge.lifecycle.hasEnded || bridge.isBroken {
            Restart.now { [weak self] in self?.restartFailed = true }
        }
    }

    /// Waits for the guest's answer off the main thread, at most Lifecycle.answerTimeout; a background task
    /// holds the time, so iOS does not suspend the process meanwhile (ADR-0032, decision 7).
    private func waitForGuest(_ wait: SessionBridge.Wait) {
        let app = UIApplication.shared
        let task = app.beginBackgroundTask(withName: "Answer to \(wait.callback)")
        let seconds = Double(Lifecycle.answerTimeout.components.seconds)
        Task.detached { [waitForAnswer] in
            let result = waitForAnswer(wait.serial, seconds)
            await MainActor.run {
                self.report(self.bridge.finished(wait, result: result, host: self.host))
                if task != .invalid { app.endBackgroundTask(task) }
            }
        }
    }
}
