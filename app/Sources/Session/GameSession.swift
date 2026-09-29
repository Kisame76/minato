import APKLibrary
import APKSession
import Foundation
import Observation
import SwiftUI
import Synchronization
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

/// One app's session in game mode (ADR-0032): the guest's start on ADR-0028's activity-host, its lifecycle
/// on the session, the menu, and the way back to the library.
@MainActor
@Observable
final class GameSession {
    let entry: LibraryEntry
    private(set) var menuIsOpen = false
    private(set) var showsHint: Bool
    /// The FPS label is on (ADR-0032, decision 9); off by default, since it makes Core Animation composite
    /// the layer. One setting for every app, kept across sessions.
    private(set) var showsFPS: Bool
    /// The label's number: the frames presented in the last second, nil until there are two samples.
    private(set) var fps: Int?
    /// The restart back to the library failed: the user opens Minato again.
    var restartFailed = false

    /// The window's size in pixels at its last layout.
    @ObservationIgnored private(set) var windowPixels = CGSize.zero
    /// The orientation iOS was asked to hold the window in.
    @ObservationIgnored private var heldOrientation: SessionOrientation?

    @ObservationIgnored private var bridge: SessionBridge
    @ObservationIgnored private let host: SessionHost
    /// The runtime's session; nil only for session-check's rounds without a guest.
    @ObservationIgnored private let runtime: RuntimeSessionHost?
    /// `apkrt_session_wait` or its stand-in, run off the main thread.
    @ObservationIgnored private let waitForAnswer: @Sendable (UInt32, Double) -> Int32
    @ObservationIgnored private var started = false
    /// `apkrt_window_presents` or its stand-in.
    @ObservationIgnored private let presents: @Sendable (UInt64) -> UInt64
    @ObservationIgnored private var frameRate = FrameRateMeter()
    @ObservationIgnored private var frameRateTask: Task<Void, Never>?
    /// Why activity-host could not start the app, as it reported it.
    @ObservationIgnored private(set) var startFailure: String?

    @ObservationIgnored private var backgroundObserver: (any NSObjectProtocol)?
    /// The view is there and the session has taken over the process (open()).
    @ObservationIgnored private var isOpen = false

    /// The guest's touches (ADR-0036, decision 3), and where they go.
    @ObservationIgnored private var touches = TouchTranslator(scale: 1)
    @ObservationIgnored private let motionSink = InputChannelSink()
    /// Ticks while the gate is open (ADR-0036, decision 2), at the rate the guest's window prefers; there
    /// once the session opens, since it runs a thread of its own.
    @ObservationIgnored private var frameClock: DisplayLinkClock?
    @ObservationIgnored private let frameClockSlot = FrameClockSlot()
    /// The guest's audio output (ADR-0036, decision 4).
    @ObservationIgnored let audio = AudioHost(sink: NoChannelAudioSink())
    #if BENCHMARK
    /// A benchmark run's hooks (ADR-0037, decision 3): no menu, FPS label or hint, and the end goes to the run.
    @ObservationIgnored private var benchmark: BenchmarkHooks?
    #endif

    /// The session of this process, for the runtime's request handler.
    private(set) weak static var current: GameSession?

    nonisolated static let hintShownKey = "sessionMenuHintShown"
    nonisolated static let showsFPSKey = "sessionShowsFPS"

    init(entry: LibraryEntry) {
        self.entry = entry
        // Game mode starts only ready apps, which have a report; activity-host reports API level 35.
        bridge = SessionBridge(lifecycle: Lifecycle(targetSdk: entry.report?.targetSdk ?? 35))
        #if DEBUG
        let withGuest = !SessionCheck.withoutGuest
        #else
        let withGuest = true
        #endif
        if withGuest {
            let runtime = RuntimeSessionHost(preferRate: { [frameClockSlot] in frameClockSlot.setPreferredRate($0) })
            self.runtime = runtime
            host = runtime
            waitForAnswer = { RuntimeSessionHost.wait(serial: $0, seconds: $1) }
            presents = { RuntimeSessionHost.presents($0) }
        } else {
            #if DEBUG
            runtime = nil
            host = NoGuestSessionHost()
            waitForAnswer = { NoGuestSessionHost.wait(serial: $0, seconds: $1) }
            presents = { NoGuestSessionHost.presents($0) }
            #else
            fatalError("every session has a guest")
            #endif
        }
        showsHint = !UserDefaults.standard.bool(forKey: Self.hintShownKey)
        showsFPS = UserDefaults.standard.bool(forKey: Self.showsFPSKey)
    }

    #if BENCHMARK
    /// Makes this session a benchmark run's, before it opens.
    func runBenchmark(_ hooks: BenchmarkHooks) {
        benchmark = hooks
        showsHint = false
        showsFPS = false
    }
    #endif

    /// The session takes over the process once its view is there: it becomes the runtime's session, writes
    /// the last session's record, and starts its frame clock, its observer of the background and the FPS
    /// label's sampler. SwiftUI may build a view's state more than once, so init does none of that.
    func open() {
        guard !isOpen else { return }
        isOpen = true
        Self.current = self
        var sink: FrameTickSink = VsyncChannelSink()
        #if BENCHMARK
        if let benchmark {
            sink = TickTee(sink, benchmark.ticks)
        } else {
            SessionEndReport.begin(entry)
        }
        #else
        SessionEndReport.begin(entry)
        #endif
        let clock = DisplayLinkClock(sink: sink)
        frameClock = clock
        frameClockSlot.set(clock)
        clock.run(bridge.lifecycle.gate == .open && !bridge.isBroken)
        #if DEBUG
        // Before the observer below, so each scene event is logged before the callbacks it causes.
        SessionCheck.observeScene()
        #endif
        // The GPU work must be scheduled before the background (ADR-0032, decision 6): the notification
        // comes on the main thread as the scene enters it, and SwiftUI's scene phase only later.
        backgroundObserver = NotificationCenter.default.addObserver(
            forName: UIScene.didEnterBackgroundNotification, object: nil, queue: nil
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.enteredBackground() }
        }
        countFrames()
    }

    /// The view is gone: the observer and the sampler stop, the frame clock pauses, and the runtime's
    /// requests no longer reach this session.
    func close() {
        guard isOpen else { return }
        isOpen = false
        if let backgroundObserver { NotificationCenter.default.removeObserver(backgroundObserver) }
        backgroundObserver = nil
        frameRateTask?.cancel()
        frameRateTask = nil
        frameClock?.run(false)
        if Self.current === self { Self.current = nil }
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

    /// The view's layer, the host side of every window the guest gets.
    func attach(_ layer: CAMetalLayer) {
        runtime?.layer = layer
        #if BENCHMARK
        benchmark?.layer(layer)
        #endif
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
        guard isOpen, let heldOrientation, heldOrientation.fits(width: windowPixels.width, height: windowPixels.height)
        else { return }
        start()
    }

    /// The window is there; the guest starts with it, or with the foreground (Lifecycle): the paths and the
    /// session first, then activity-host off the main thread, then its first events.
    private func start() {
        guard !started else { return }
        started = true
        guard let runtime else { return deliver(.start) }
        guard let (start, configuration) = guestStart(scale: runtime.layer?.contentsScale ?? 1) else {
            return couldNotStart("it is not a NativeActivity app")
        }
        #if DEBUG
        SessionCheck.guestStarts()
        #endif
        if let failure = runtime.begin(start, configuration: configuration) {
            return couldNotStart(failure)
        }
        Task.detached {
            let failure = RuntimeSessionHost.startProgram(start)
            await MainActor.run {
                if let failure { self.couldNotStart(failure) } else { self.deliver(.start) }
            }
        }
    }

    /// What activity-host needs for this app (ADR-0028, decisions 3 and 14), with the window's size as the
    /// screen's in the session's orientation. Nil for an app without a NativeActivity.
    private func guestStart(scale: CGFloat) -> (GuestStart, GuestConfiguration)? {
        guard case .nativeActivity(let library) = entry.report?.runtime, let paths = try? LibraryPaths.standard(),
              let bundle = Bundle.main.resourceURL
        else { return nil }
        let data = paths.dataDirectory(for: entry.id)
        let start = GuestStart(
            package: entry.id, library: library, function: entry.report?.nativeFunction,
            apk: paths.versionDirectory(for: entry).appending(path: "base.apk").path,
            appLibraries: paths.libraryDirectory(for: entry).path,
            data: data.appending(path: "data").path, external: data.appending(path: "external").path,
            obb: data.appending(path: "obb").path, guestSystem: bundle.appending(path: "Guest").path)
        // Android's locale is the first preferred language, with the region's settings where it names none.
        let language = Locale(identifier: Locale.preferredLanguages.first ?? "en")
        let configuration = GuestConfiguration(
            scale: Double(scale), width: Int32(windowPixels.width.rounded()), height: Int32(windowPixels.height.rounded()),
            dark: UITraitCollection.current.userInterfaceStyle == .dark,
            refreshRate: Int32((UIApplication.shared.connectedScenes.first as? UIWindowScene)?.screen.maximumFramesPerSecond ?? 60),
            languageCode: language.language.languageCode?.identifier,
            regionCode: language.region?.identifier ?? Locale.current.region?.identifier)
        return (start, configuration)
    }

    /// The guest cannot start: the library tells why at its next start (decision 8).
    private func couldNotStart(_ reason: String) {
        log.error("\(self.entry.id, privacy: .public) cannot start: \(reason, privacy: .public)")
        #if DEBUG
        SessionCheck.event("could not start \(reason)")
        #endif
        #if BENCHMARK
        if let benchmark { return benchmark.ended(reason) }
        #endif
        startFailure = reason
        SessionEndReport.startFailed(reason)
        Restart.now { [weak self] in self?.restartFailed = true }
    }

    /// The program ended, after onDestroy or by itself (ADR-0032, decision 7): back to the library. A crash
    /// ends the process without this.
    func programEnded(status: Int32) {
        log.info("\(self.entry.id, privacy: .public) ended with status \(status)")
        #if DEBUG
        SessionCheck.event("program ended \(status)")
        #endif
        #if BENCHMARK
        if let benchmark { return benchmark.ended(status == 0 ? nil : "the variant ended with status \(status)") }
        #endif
        Restart.now { [weak self] in self?.restartFailed = true }
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
        #if BENCHMARK
        guard benchmark == nil else { return }
        #endif
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

    /// "Show FPS" or "Hide FPS" in the menu.
    func showFPS(_ on: Bool) {
        showsFPS = on
        UserDefaults.standard.set(on, forKey: Self.showsFPSKey)
        #if DEBUG
        SessionCheck.event(on ? "fps shown" : "fps hidden")
        #endif
        countFrames()
    }

    /// While the label is on, samples the presents of the guest's window about once a second.
    private func countFrames() {
        frameRateTask?.cancel()
        frameRate = FrameRateMeter()
        fps = nil
        guard isOpen, showsFPS else { return }
        frameRateTask = Task { [weak self] in
            while !Task.isCancelled, self != nil {
                self?.sampleFrameRate()
                try? await Task.sleep(for: .seconds(1))
            }
        }
    }

    private func sampleFrameRate() {
        let window = bridge.window
        let count = presents(window)
        fps = frameRate.sample(window: window, presents: count, at: Double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) / 1e9)
        #if DEBUG
        SessionCheck.event("fps window \(window) presents \(count) value \(fps.map(String.init) ?? "none")")
        #endif
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
        motionSink.channel = bridge.inputChannel ?? 0
        motionSink.post(events)
    }

    /// A request of the guest (ADR-0028, decision 4), on the main thread.
    func received(_ request: SessionRequest) {
        #if DEBUG
        SessionCheck.event("request \(request)")
        #endif
        switch request {
        case .finish:
            deliver(.finish)
        case .keepScreenOn(let on):
            UIApplication.shared.isIdleTimerDisabled = on
        case .frameRate:
            // The request handler gave it to the frame clock already.
            break
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
        frameClock?.run(bridge.lifecycle.gate == .open && !bridge.isBroken)
        // A gesture that is open when the focus goes ends with a CANCEL (ADR-0036, decision 3).
        if handled.callbacks.contains(.onWindowFocusChanged(false)) {
            post(touches.focusLost(at: Double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) / 1e9))
        }
        if let wait = handled.wait { waitForGuest(wait) }
        if bridge.isBroken {
            log.error("\(self.entry.id, privacy: .public): the session's channel broke")
        }
        if bridge.lifecycle.hasEnded || bridge.isBroken {
            #if BENCHMARK
            if let benchmark { return benchmark.ended(bridge.isBroken ? "the session's channel broke" : nil) }
            #endif
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
            #if DEBUG
            let start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
            #endif
            let result = waitForAnswer(wait.serial, seconds)
            #if DEBUG
            SessionCheck.answer(wait.callback, result: result, since: start)
            #endif
            await MainActor.run {
                self.report(self.bridge.finished(wait, result: result, host: self.host))
                if task != .invalid { app.endBackgroundTask(task) }
            }
        }
    }
}

/// The frame clock for the runtime's request handler, which calls it on the guest's thread without blocking:
/// empty until the session opens.
private final class FrameClockSlot: Sendable {
    private let clock = Mutex<DisplayLinkClock?>(nil)

    func set(_ clock: DisplayLinkClock) {
        self.clock.withLock { $0 = clock }
    }

    func setPreferredRate(_ rate: Float) {
        clock.withLock { $0 }?.setPreferredRate(rate)
    }
}
