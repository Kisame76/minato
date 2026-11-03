import APKKit
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
    /// The menu opened for a back at the task's root (ADR-0041 decision 10): its Quit leaves the app stopped.
    private(set) var menuIsAtRoot = false
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
    /// The view's layer, whose opacity follows the window's (ADR-0030 V5).
    @ObservationIgnored private weak var layer: CAMetalLayer?
    /// The session's view, which shows the guest's window in size compat in its letterbox (ADR-0041, decision 12 b).
    @ObservationIgnored private weak var layersView: SessionLayersView?
    /// What the guest asks to open (ADR-0062), over game mode.
    @ObservationIgnored private lazy var opens = OpenPresenter(
        dataFolder: (try? LibraryPaths.standard())?.dataDirectory(for: entry.id).appending(path: "data")
    ) { [weak self] id, outcome in self?.openAnswered(id, outcome) }
    /// What the guest asks the user to pick (ADR-0057), over game mode.
    @ObservationIgnored private lazy var picks: PickPresenter = {
        let data = (try? LibraryPaths.standard())?.dataDirectory(for: entry.id)
        return PickPresenter(dataFolder: data?.appending(path: "data"), download: data?.appending(path: "download")) {
            [weak self] id, picked in self?.pickAnswered(id, picked)
        }
    }()
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
    /// Whether a touch was a tap outside the focused field, which hides the keyboard (ADR-0068 a).
    @ObservationIgnored private var outsideTap = OutsideTap()
    @ObservationIgnored private let motionSink = InputChannelSink()
    /// The hardware keyboard's keys (ADR-0068 step 7c2), and where they go.
    @ObservationIgnored private var keyboard = HardwareKeyboard()
    @ObservationIgnored private let keySink = KeyChannelSink()
    /// The guest's input method on UIKit's keyboard (ADR-0068 step 2c): its editor, the view that has the keyboard, and
    /// where the typed text goes.
    @ObservationIgnored private var softKeyboard: SoftKeyboard
    @ObservationIgnored private weak var textView: SoftKeyboardView?
    @ObservationIgnored private let textSink = TextChannelSink()
    /// Ticks while the gate is open (ADR-0036, decision 2), at the rate the guest's window prefers; there
    /// once the session opens, since it runs a thread of its own.
    @ObservationIgnored private var frameClock: DisplayLinkClock?
    @ObservationIgnored private let frameClockSlot = FrameClockSlot()
    /// The guest's audio output (ADR-0036, decision 4).
    @ObservationIgnored let audio = AudioHost(sink: RuntimeAudioSink())
    /// The iPhone's battery for the guest (the battery, gap 1).
    @ObservationIgnored private let battery = BatteryHost()
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
        // LatinIME's Enter and delete are keys for an app before Jelly Bean.
        softKeyboard = SoftKeyboard(targetSdk: entry.report?.targetSdk ?? 35)
        #if DEBUG
        let withGuest = !SessionCheck.withoutGuest
        #else
        let withGuest = true
        #endif
        if withGuest {
            let runtime = RuntimeSessionHost(
                dataFolder: (try? LibraryPaths.standard())?.dataDirectory(for: entry.id).appending(path: "data"),
                preferRate: { [frameClockSlot] in frameClockSlot.setPreferredRate($0) })
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

    /// The guest's presents in this session, through every window it had (ADR-0041 d).
    var guestPresents: UInt64 {
        (runtime?.presentsOfRemovedWindows ?? 0) + (bridge.window == 0 ? 0 : presents(bridge.window))
    }

    /// The last rate the guest asked for with `ANativeWindow_setFrameRate`, 0 when it asked none.
    var guestAskedRate: Float { frameClockSlot.askedRate }
    #endif

    /// The session takes over the process once its view is there: it becomes the runtime's session, writes
    /// the last session's record, and starts the battery's reports, its frame clock, its observer of the background
    /// and the FPS label's sampler. SwiftUI may build a view's state more than once, so init does none of that.
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
        battery.start()
        let clock = DisplayLinkClock(sink: sink)
        frameClock = clock
        frameClockSlot.set(clock)
        clock.run(framesRun)
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
        battery.stop()
        if softKeyboard.isShown { textView?.hide() }
        frameRateTask?.cancel()
        frameRateTask = nil
        frameClock?.run(false)
        if Self.current === self { Self.current = nil }
    }

    /// The orientation the session is to hold (ADR-0032, decision 5), or whether it turns (ADR-0038).
    var orientation: SessionOrientation {
        #if DEBUG
        if let requested = SessionCheck.requestedOrientation { return requested }
        #endif
        let report = entry.report
        return SessionOrientation(screenOrientation: report?.screenOrientation, configChanges: report?.configChanges,
                                  targetSdk: report?.targetSdk)
    }

    /// iOS was asked to hold the window in `orientation`, which turns it after a while; a session that turns
    /// turns its window at each layout in another size.
    func holds(_ orientation: SessionOrientation) {
        heldOrientation = orientation
        bridge.turns = orientation == .turns
        bridge.resizes = SessionBridge.resizes(configChanges: entry.report?.configChanges,
                                               targetSdk: entry.report?.targetSdk)
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

    /// The session's view, once, as it is made.
    func attach(_ view: SessionLayersView) {
        layersView = view
        view.onWindowMoved = { RuntimeSessionHost.windowsMoved() }
    }

    /// The view's layer, the host side of every window the guest gets, and its SurfaceViews' layers (ADR-0043 c4).
    func attach(_ layer: CAMetalLayer, surfaces: SurfaceLayers) {
        self.layer = layer
        runtime?.layer = layer
        runtime?.surfaces = surfaces
        #if BENCHMARK
        benchmark?.layer(layer)
        surfaces.onAdd = { [weak self] in self?.benchmark?.surface($0) }
        #endif
    }

    /// The soft keyboard's view, once, as the session's view is made (ADR-0068 step 2c).
    func attach(_ textView: SoftKeyboardView) {
        self.textView = textView
        textView.onInsert = { [weak self] text in
            guard let self else { return }
            type(softKeyboard.insert(text))
        }
        textView.onDelete = { [weak self] in
            guard let self else { return }
            type(softKeyboard.deleteBackward())
        }
        textView.onReturn = { [weak self] in
            guard let self else { return }
            type(softKeyboard.returnKey())
        }
        textView.onDismissed = { [weak self] in self?.keyboardDismissed() }
        textView.onSelect = { [weak self] offset in
            guard let self else { return }
            type(softKeyboard.select(at: offset))
        }
    }

    /// The window was laid out: the size the guest starts with, and the next window's; while the guest has a
    /// window, a change it handles (ADR-0038, decision 1.4; ADR-0041, decision 12 a), or size compat, whose letterbox
    /// the view and the touches take (decision 12 b).
    func windowLaidOut(_ pixels: CGSize) {
        windowPixels = pixels
        let size = PixelSize(width: Int32(pixels.width.rounded()), height: Int32(pixels.height.rounded()))
        // The bars at this layout's turn, which a turn posts to the guest (ADR-0041 decision 8, finding 3).
        bridge.systemBars = barInsets(scale: runtime?.layer?.contentsScale ?? 1).systemBars
        let handled = bridge.windowLaidOut(size, host: host)
        layersView?.letterbox = bridge.letterbox
        touches.letterbox = bridge.letterbox
        // A layout that turned nothing must not report again a session that has ended.
        if !handled.callbacks.isEmpty { report(handled) }
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
    /// session first, then the program off the main thread, activity-host or a Java app's app_process64, then its
    /// first events.
    private func start() {
        guard !started else { return }
        started = true
        guard let runtime else { return deliver(.start) }
        guard let (start, configuration) = guestStart(scale: runtime.layer?.contentsScale ?? 1) else {
            return couldNotStart("its base.apk has no valid SHA-256, which names its code path")
        }
        if case .art = entry.report?.runtime, let failure = prepareJavaStart(start) {
            return couldNotStart(failure)
        }
        frameClock?.setUnlock(Float(start.frameRateUnlock))
        #if DEBUG
        SessionCheck.guestStarts()
        #endif
        SessionLogTap.begin(package: entry.id)
        if let failure = runtime.begin(start, configuration: configuration) {
            return couldNotStart(failure)
        }
        // The records of the clones of the user's files and their watches, once the mounts are made, and the app's
        // versions kept before the guest's cleaning (ADR-0057's addendum, point 4).
        if let data = (try? LibraryPaths.standard())?.dataDirectory(for: entry.id) {
            WriteBackHost.shared.begin(package: entry.id, download: data.appending(path: "download"))
        }
        let audio = audio
        Task.detached {
            // Before the guest runs, so its first stream opens on the host.
            audio.serveRuntime()
            // After the app's versions are kept, before the guest's cleaning (ADR-0057's addendum, point 4).
            WriteBackHost.shared.waitUntilSecured()
            let failure = RuntimeSessionHost.startProgram(start)
            await MainActor.run {
                if let failure { self.couldNotStart(failure) } else { self.deliver(.start) }
            }
        }
    }

    /// The guest's insets at the start: the scene's safe area at its turn, the window's pixels and the model's housing.
    private func barInsets(scale: CGFloat) -> GuestInsets {
        let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene
        let safe = scene?.keyWindow?.safeAreaInsets ?? .zero
        let orientation: InterfaceOrientations = switch scene?.effectiveGeometry.interfaceOrientation {
        case .landscapeLeft: .landscapeLeft
        case .landscapeRight: .landscapeRight
        default: .portrait
        }
        let housing = GuestInsets.Housing(environment: ProcessInfo.processInfo.environment,
                                          machine: AppReport.Device.machine)
        return GuestInsets(safeArea: .init(top: safe.top, left: safe.left, bottom: safe.bottom, right: safe.right),
                           size: PixelSize(width: Int32(windowPixels.width.rounded()),
                                           height: Int32(windowPixels.height.rounded())),
                           orientation: orientation, device: .init(housing: housing, scale: scale))
    }

    /// What the program needs for this app, activity-host for a NativeActivity app (ADR-0028, decisions 3 and 14)
    /// or app_process64 for a Java app (ADR-0039 c5), with the window's size as the screen's in the session's
    /// orientation. Nil for an app without base.apk's digest, which names its code path (ADR-0039, decision 6).
    private func guestStart(scale: CGFloat) -> (GuestStart, GuestConfiguration)? {
        guard let kind = entry.report?.runtime, let paths = try? LibraryPaths.standard(),
              let bundle = Bundle.main.resourceURL, let digest = entry.base?.sha256
        else { return nil }
        // An app imported before decision 6 gets installd's cache and code_cache now.
        do {
            try paths.prepareData(for: entry.id)
        } catch {
            log.error("\(self.entry.id, privacy: .public): its data folders: \(error.localizedDescription, privacy: .public)")
        }
        let data = paths.dataDirectory(for: entry.id)
        #if BENCHMARK
        let environment = BenchmarkMode.guestEnvironment
        #elseif DEBUG
        let environment = Self.debugGuestEnvironment()
        #else
        let environment: [String] = []
        #endif
        // The bars the guest's floating windows are laid out inside (ADR-0041 decision 8, finding 3).
        let bars = barInsets(scale: scale).barInsetsEnvironment
        let guestEnvironment = environment + [bars]
        #if DEBUG
        SessionCheck.event("guest \(bars)")
        #endif
        let version = paths.versionDirectory(for: entry)
        // An installed set keeps each split beside base.apk as split_<name>.apk (ADR-0048 decision 9).
        let splits = ((try? FileManager.default.contentsOfDirectory(atPath: version.path)) ?? [])
            .filter { $0.hasPrefix("split_") && $0.hasSuffix(".apk") }.sorted()
            .map { version.appending(path: $0).path }
        let start: GuestStart?
        switch kind {
        case .nativeActivity(let library):
            start = GuestStart(
                package: entry.id, library: library, function: entry.report?.nativeFunction,
                apk: version.appending(path: "base.apk").path, apkSHA256: digest,
                appLibraries: paths.libraryDirectory(for: entry).path,
                data: data.appending(path: "data").path, deviceData: data.appending(path: "data_de").path,
                external: data.appending(path: "external").path, obb: data.appending(path: "obb").path,
                download: data.appending(path: "download").path,
                guestSystem: bundle.appending(path: "Guest").path, splits: splits, environment: guestEnvironment)
        case .art:
            // An unsigned APK has no signers.json.
            let signers = version.appending(path: SignersFile.fileName)
            // The compiled code the library kept for this boot class path (ADR-0053, M6 c), if any; ART rejects a stale
            // one anyway, but not mounting it spares the load.
            let oat = GuestStart.bootClassPathChecksum(guestSystem: bundle.appending(path: "Guest").path)
                .flatMap { paths.oatFolders(for: entry, bootClassPath: $0) }
                .map { GuestStart.OatFolders(code: $0.code.path, containers: $0.containers.path) }
            start = GuestStart(
                javaApp: entry.id, apk: version.appending(path: "base.apk").path, apkSHA256: digest,
                signers: FileManager.default.fileExists(atPath: signers.path) ? signers.path : nil,
                appLibraries: paths.libraryDirectory(for: entry).path,
                data: data.appending(path: "data").path, deviceData: data.appending(path: "data_de").path,
                external: data.appending(path: "external").path, obb: data.appending(path: "obb").path,
                download: data.appending(path: "download").path,
                guestData: paths.guestDataDirectory(for: entry.id).path,
                guestSystem: bundle.appending(path: "Guest").path,
                // The kept pack slices and the feature modules' splits, which prepareJavaStart lays out in
                // local_testing (ADR-0050 decisions 2 and 3).
                localTesting: entry.usesLocalTesting, splits: splits, environment: guestEnvironment, oat: oat)
        }
        guard var start else { return nil }
        // ES 3.1 and up on ANGLE's Vulkan backend, by the installed set's highest glEsVersion (ADR-0044 d)).
        start.angleBackend = ANGLEBackend(requiredGLESVersion: entry.report?.requiredGLESVersion)
        // The app's sockets without INTERNET answer EPERM, as on Android (ADR-0054 c2a).
        start.internet = entry.report?.usesInternet ?? true
        let screenMaximum = (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.screen
            .maximumFramesPerSecond ?? 60
        #if BENCHMARK
        // A benchmark run is judged at the app's own settings; only a capped run takes its FPS unlock, as an app's
        // profile hands it (ADR-0066 step 4).
        if let run = BenchmarkMode.run {
            let unlock = BenchmarkConditions.frameRateUnlock(for: run.configuration.scene)
            start.apply(AppProfile(frameRate: AppProfile.FrameRate(rawValue: unlock)), screenMaximum: screenMaximum)
        }
        #else
        // The user's choices for this app (ADR-0065, ADR-0066).
        start.apply(AppProfile.load(for: entry.id, in: paths), screenMaximum: screenMaximum)
        #endif
        #if DEBUG
        SessionCheck.event("profile device \(start.deviceProfile ?? "default") unlock \(start.frameRateUnlock)")
        #endif
        // Android's locale is the first preferred language, with the region's settings where it names none.
        let language = Locale(identifier: Locale.preferredLanguages.first ?? "en")
        let configuration = GuestConfiguration(
            scale: Double(scale), width: Int32(windowPixels.width.rounded()), height: Int32(windowPixels.height.rounded()),
            dark: UITraitCollection.current.userInterfaceStyle == .dark,
            refreshRate: Int32(screenMaximum),
            languageCode: language.language.languageCode?.identifier,
            regionCode: language.region?.identifier ?? Locale.current.region?.identifier)
        return (start, configuration)
    }

    /// A Java app's /data in Caches and its read-only files, at every start, since iOS may empty Caches (ADR-0039
    /// c5): nil once they are there, else why the app cannot start.
    private func prepareJavaStart(_ start: GuestStart) -> String? {
        do {
            _ = try LibraryPaths.standard().prepareJavaStart(for: entry, codePath: start.codePath)
            return nil
        } catch {
            return "its folders could not be made: \(error.localizedDescription)"
        }
    }

    #if DEBUG
    /// tmp/guest-environment's MINATO_* entries for the guest (ADR-0030 V5's data step), such as libvulkan's
    /// MINATO_VULKAN_BGRA8_FIRST=1: a restarted process gets no launch arguments, so a file in Minato's
    /// container carries them. Each entry and each skipped line is logged. Release builds have none of it.
    private static func debugGuestEnvironment() -> [String] {
        let file = URL.temporaryDirectory.appending(path: "guest-environment")
        guard let text = try? String(contentsOf: file, encoding: .utf8) else { return [] }
        let parsed = DebugGuestEnvironment.parse(text)
        for entry in parsed.entries {
            log.notice("guest environment (Debug): \(entry, privacy: .public)")
            SessionCheck.event("guest environment \(entry)")
        }
        for skipped in parsed.skipped {
            log.error("guest environment (Debug): \(skipped, privacy: .public) skipped, only MINATO_* keys pass")
            SessionCheck.event("guest environment skipped \(skipped)")
        }
        return parsed.entries
    }
    #endif

    /// The guest cannot start: the library tells why at its next start (decision 8).
    private func couldNotStart(_ reason: String) {
        log.error("\(self.entry.id, privacy: .public) cannot start: \(reason, privacy: .public)")
        #if DEBUG
        SessionCheck.event("could not start \(reason)")
        #endif
        #if BENCHMARK
        if let benchmark { return benchmark.ended(nil, reason) }
        #endif
        startFailure = reason
        SessionEndReport.startFailed(reason)
        restart()
    }

    /// Back to the library, once the exports to Photos under way are done or 5 s have passed (ADR-0057 step e) and the
    /// app's edits of the user's files are written back, 5 s at most (its addendum).
    private func restart() {
        // The end is the user's or the app's: no notice at the library's next start (ADR-0032, decision 8).
        SessionEndReport.ended()
        SessionLogTap.end()
        Task { [weak self, photos = runtime?.photos] in
            await photos?.settle(within: 5)
            await WriteBackHost.shared.endSession()
            Restart.now { [weak self] in self?.restartFailed = true }
        }
    }

    /// The program ended, after onDestroy or by itself (ADR-0032, decision 7): back to the library. A crash
    /// ends the process without this.
    func programEnded(status: Int32) {
        log.info("\(self.entry.id, privacy: .public) ended with status \(status)")
        SessionEndReport.ended()
        #if DEBUG
        SessionCheck.event("program ended \(status)")
        SessionCheck.event("angle swapchains \(minato_graphics_angle_vulkan_swapchains())")
        #endif
        #if BENCHMARK
        if let benchmark {
            return benchmark.ended(status, status == 0 ? nil : "the variant ended with status \(status)")
        }
        #endif
        restart()
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

    func openMenu(atRoot: Bool = false) {
        guard !menuIsOpen else { return }
        #if BENCHMARK
        guard benchmark == nil else { return }
        #endif
        menuIsOpen = true
        menuIsAtRoot = atRoot
        #if DEBUG
        SessionCheck.event("menu opened")
        #endif
        deliver(.menuOpened)
    }

    func closeMenu() {
        guard menuIsOpen else { return }
        menuIsOpen = false
        menuIsAtRoot = false
        #if DEBUG
        SessionCheck.event("menu closed")
        #endif
        deliver(.menuClosed)
        // A sheet may have taken the keyboard's focus.
        if softKeyboard.isShown, let textView, !textView.isFirstResponder { textView.becomeFirstResponder() }
    }

    /// The system's back (ADR-0041 decision 10), as Android's back button gives it, for the edge gesture (step b2),
    /// once a menu that is open has given the focus back. With UIKit's keyboard up it only puts the keyboard away, as
    /// Android's input method takes the first BACK.
    func back() {
        let keyboardTook = softKeyboard.takesBack()
        closeMenu()
        if keyboardTook {
            textView?.hide()
            #if DEBUG
            SessionCheck.event("back hid the keyboard")
            #endif
            keyboardHidden()
            return
        }
        #if DEBUG
        SessionCheck.event("back")
        #endif
        let wasBroken = bridge.isBroken
        let handled = bridge.back(.commit, progress: 0, edge: 0, host: host)
        if !wasBroken, bridge.isBroken { report(handled) }
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

    /// The window or SurfaceView surface with the most presents (ADR-0041 decision 3; ADR-0043 c4).
    private func sampleFrameRate() {
        let (window, count) = ([bridge.window] + bridge.surfaceWindows).map { ($0, presents($0)) }
            .max { $0.1 < $1.1 } ?? (0, 0)
        fps = frameRate.sample(window: window, presents: count, at: Double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) / 1e9)
        #if DEBUG
        SessionCheck.event("fps window \(window) presents \(count) value \(fps.map(String.init) ?? "none")")
        #endif
    }

    /// Quit in the menu: the guest ends, then Minato restarts into the library. In the menu of a back at the task's
    /// root, the app stops as Android leaves a task in the back, and ends without onDestroy (ADR-0041 decision 10).
    func quit() {
        let atRoot = menuIsAtRoot
        menuIsOpen = false
        menuIsAtRoot = false
        #if DEBUG
        SessionCheck.event(atRoot ? "quit at root" : "quit")
        #endif
        deliver(atRoot ? .quitAtRoot : .quit)
    }

    /// A touch callback of the window, with its pixels per point.
    func touched(_ phase: TouchPhase, _ changes: [TouchChange], scale: Double) {
        #if DEBUG
        if phase == .began { changes.forEach { SessionCheck.touchClock($0.samples.last?.timestamp ?? 0) } }
        #endif
        touches.scale = scale
        let decision = outsideTap.touched(phase, changes, keyboardShown: softKeyboard.isShown)
        // A touch that may put the keyboard away waits in outsideTap; the tap that does never reaches the guest.
        for callback in outsideTap.release() { post(touches.translate(callback.phase, callback.changes)) }
        switch decision {
        case .none:
            break
        case .hide:
            hideForTap()
        case .hideAfterGrace(let token):
            DispatchQueue.main.asyncAfter(deadline: .now() + OutsideTap.grace) { [weak self] in
                guard let self, outsideTap.graceEnded(token) else { return }
                hideForTap()
            }
        }
    }

    /// A tap outside the field (ADR-0068 a): UIKit's keyboard goes as by the user's hide, which the guest hears of; the
    /// tap itself does not reach it.
    private func hideForTap() {
        #if DEBUG
        SessionCheck.event("keyboard tapped away")
        #endif
        _ = textView?.resignFirstResponder()
    }

    /// A press callback of a hardware keyboard's keys, on the main thread.
    func keyed(_ phase: KeyPhase, _ presses: [HardwareKeyPress]) {
        let events: [KeyboardEvent]
        switch phase {
        case .down, .up:
            events = presses.flatMap {
                keyboard.press(usage: $0.usage, down: phase == .down, time: HardwareKeyboard.nanoseconds($0.timestamp))
            }
        case .cancelled:
            events = keyboard.releaseAll(time: HardwareKeyboard.nanoseconds(presses.last?.timestamp ?? 0))
        }
        guard !events.isEmpty else { return }
        keySink.channel = bridge.inputChannel ?? 0
        keySink.post(events)
    }

    private func post(_ events: [MotionEvent]) {
        guard !events.isEmpty else { return }
        motionSink.channel = bridge.inputChannel ?? 0
        motionSink.post(events)
    }

    #if DEBUG
    /// The layer's opacity, as session-check logs it.
    var layerIsOpaque: Bool { layer?.isOpaque ?? false }

    /// The guest's size and the letterbox, as session-check logs them (ADR-0041, decision 12).
    var sizeLine: String {
        let guest = bridge.guestSize
        guard let box = bridge.letterbox else { return "guest \(guest.width)x\(guest.height) letterbox none" }
        return "guest \(guest.width)x\(guest.height) letterbox \(box.frame.left),\(box.frame.top),\(box.frame.right),"
            + "\(box.frame.bottom) scale " + String(format: "%.4f", box.scale)
    }
    #endif

    /// A request of the guest (ADR-0028, decision 4), on the main thread.
    func received(_ request: SessionRequest) {
        #if DEBUG
        SessionCheck.event("request \(request.checkLine)")
        #endif
        switch request {
        case .finish:
            deliver(.finish)
        case .keepScreenOn(let on):
            UIApplication.shared.isIdleTimerDisabled = on
        case .frameRate:
            // The request handler gave it to the frame clock already.
            break
        case .windowOpaque(let opaque):
            if let runtime { runtime.windowOpaque = opaque } else { layer?.isOpaque = opaque }
            #if DEBUG
            SessionCheck.event("layer opaque \(layerIsOpaque ? 1 : 0)")
            #endif
        case .startFailed(let reason):
            log.error("\(self.entry.id, privacy: .public) cannot start: \(reason, privacy: .public)")
            startFailure = reason
            SessionEndReport.startFailed(reason)
        case .open(let id, let text):
            opens.requested(id: id, text: text, inBackground: bridge.isInBackground)
        case .openCancel(let id):
            opens.cancelled(id: id)
        case .pick(let id, let text):
            picks.requested(id: id, text: text)
        case .pickCancel(let id):
            picks.cancelled(id: id)
        case .ime(let serial, let text):
            imeRequested(serial: serial, text: text)
        case .backAtRoot:
            // Where Android moves the task to the back, which would end Minato's session (ADR-0041 decision 10).
            openMenu(atRoot: true)
        }
    }

    /// The guest's input method (ADR-0068 step 2c): UIKit's keyboard follows its served editor, the bar above it the
    /// field's text (keyboard plan b), and a tap outside the keyboard and the field puts it away (plan a). An extract's
    /// text is never logged, only its length.
    private func imeRequested(serial: Int64, text: String) {
        guard let request = ImeRequest(text: text, serial: serial) else { return }
        switch request {
        case .show:
            outsideTap.showRequested()
        case .start, .stop:
            outsideTap.field = nil  // the new editor's view comes with its next field
        case .field:
            _ = softKeyboard.handle(request)
            outsideTap.field = softKeyboard.field.map(inPoints)
            return
        case .extract:
            _ = softKeyboard.handle(request)
            #if DEBUG
            SessionCheck.event("keyboard \(softKeyboard.bar.map(String.init(describing:)) ?? "no text bar")")
            #endif
            textView?.showText(softKeyboard.bar)
            return
        case .hide:
            break
        }
        guard let command = softKeyboard.handle(request) else { return }
        #if DEBUG
        SessionCheck.event("keyboard \(command)")
        #endif
        switch command {
        case .show(let traits): textView?.show(traits)
        case .update(let traits): textView?.update(traits)
        case .hide: textView?.hide()
        case .release:
            // A restart's start comes within this, so the keyboard stays up through it (SoftKeyboard).
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
                guard let self, softKeyboard.releaseExpired() else { return }
                textView?.hide()
            }
        }
    }

    /// A rectangle in the window's pixels in the session view's points.
    private func inPoints(_ rect: FieldRect) -> FieldRect {
        let scale = Double(runtime?.layer?.contentsScale ?? 1)
        return FieldRect(left: rect.left / scale, top: rect.top / scale, right: rect.right / scale,
                         bottom: rect.bottom / scale)
    }

    /// UIKit took the keyboard away on its own: a tap outside the field (hideForTap), an iPad's dismiss key. The menu's
    /// sheet and the background give it back afterwards, so they change nothing.
    private func keyboardDismissed() {
        guard !menuIsOpen, UIApplication.shared.applicationState == .active, softKeyboard.isShown else { return }
        softKeyboard.userHid()
        textView?.hide()
        #if DEBUG
        SessionCheck.event("keyboard dismissed")
        #endif
        keyboardHidden()
    }

    /// The guest hears of the user's hide as Android's input method hiding itself on BACK (ADR-0068): an app that tracks
    /// its keyboard itself asks for it again at the next tap.
    private func keyboardHidden() {
        let wasBroken = bridge.isBroken
        let handled = bridge.keyboardHidden(host: host)
        if !wasBroken, bridge.isBroken { report(handled) }
    }

    private func type(_ records: [TextRecord]) {
        guard !records.isEmpty else { return }
        textSink.channel = bridge.inputChannel ?? 0
        textSink.post(records)
    }

    /// The answer to a pick request (ADR-0057): the number of files the manifest names, nil for a cancel.
    private func pickAnswered(_ id: UInt64, _ picked: Int?) {
        let outcome = picked.map { "\($0) picked" } ?? "cancelled"
        log.info("\(self.entry.id, privacy: .public): pick \(id) \(outcome, privacy: .public)")
        let wasBroken = bridge.isBroken
        let handled = bridge.pickAnswered(id: id, picked: picked, host: host)
        if !wasBroken, bridge.isBroken { report(handled) }
    }

    /// UIKit's keyboard covers `pixels` of the session's view (ADR-0068 step 2d): every window's ime() inset.
    func keyboardCovers(_ pixels: Int32) {
        #if DEBUG
        SessionCheck.event("keyboard covers \(pixels)")
        #endif
        let wasBroken = bridge.isBroken
        let handled = bridge.keyboardCovers(pixels: pixels, host: host)
        if !wasBroken, bridge.isBroken { report(handled) }
    }

    /// The answer to an open request (ADR-0062), which the guest's framework activity finishes with.
    private func openAnswered(_ id: UInt64, _ outcome: OpenOutcome) {
        log.info("\(self.entry.id, privacy: .public): open \(id) \(String(describing: outcome), privacy: .public)")
        let wasBroken = bridge.isBroken
        let handled = bridge.openAnswered(id: id, outcome, host: host)
        if !wasBroken, bridge.isBroken { report(handled) }
    }

    /// A SurfaceView's request (ADR-0043 c4), on the main thread, in the order the runtime took them.
    func surfaceRequested(_ request: SurfaceRequest) {
        #if DEBUG
        SessionCheck.event("surface \(request.operation) token \(request.token) z \(request.z)")
        #endif
        let wasBroken = bridge.isBroken
        let handled = bridge.surfaceRequested(request, host: host)
        // A post that broke the channel ends the session, as a callback's does.
        if !wasBroken, bridge.isBroken { report(handled) }
    }

    private func deliver(_ event: Lifecycle.Event) {
        report(bridge.handle(event, host: host))
    }

    /// Whether the frame clock runs: the lifecycle's rule for the app's kind (ADR-0041, decision 2), a Java app's
    /// frames while the scene is in the foreground (App's c6 host fixes), and none once the channel broke.
    private var framesRun: Bool {
        let javaApp = if case .art = entry.report?.runtime { true } else { false }
        return !bridge.isBroken && bridge.lifecycle.framesRun(javaApp: javaApp)
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
        #if DEBUG
        // The bridge's own waits at the background come after the callbacks they answered.
        for answer in handled.answers {
            SessionCheck.answer(answer.callback, result: answer.result, seconds: answer.seconds)
        }
        #endif
        if handled.callbacks.contains(.onCreate) { guestStarted() }
        frameClock?.run(framesRun)
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
            if let benchmark { return benchmark.ended(nil, bridge.isBroken ? "the session's channel broke" : nil) }
            #endif
            restart()
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
    private let asked = Mutex<Float>(0)

    /// The last rate the guest asked for, 0 when it asked none: a benchmark run's requested_rate.
    var askedRate: Float { asked.withLock { $0 } }

    func set(_ clock: DisplayLinkClock) {
        self.clock.withLock { $0 = clock }
    }

    func setPreferredRate(_ rate: Float) {
        if rate > 0 { asked.withLock { $0 = rate } }
        clock.withLock { $0 }?.setPreferredRate(rate)
    }
}
