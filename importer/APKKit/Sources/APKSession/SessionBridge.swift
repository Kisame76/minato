/// A window's size in pixels: the layer's bounds × contentsScale (ADR-0032, decision 5).
public struct PixelSize: Sendable, Equatable {
    public var width: Int32
    public var height: Int32

    public init(width: Int32, height: Int32) {
        self.width = width
        self.height = height
    }
}

/// An event for activity-host, one per NativeActivity callback (ADR-0028, decision 2), with its
/// arguments. The app turns it into `apkrt_session_post`'s type and arguments.
public enum SessionEvent: Sendable, Equatable {
    case create
    case start
    case resume
    case pause
    case stop
    case destroy
    case saveInstanceState
    case windowFocusChanged(Bool)
    case nativeWindowCreated(window: UInt64, width: Int32, height: Int32)
    case nativeWindowDestroyed(window: UInt64)
    case lowMemory
    /// ADR-0028 d)'s callbacks beyond decision 7, in the places Android 15 calls them: the input queue after
    /// the first onResume (with the input channel `apkrt_input_add` gave, ADR-0036 b), and after each new
    /// window a resize to the same size, the content's rectangle when it changed, and redraw needed.
    case inputQueueCreated(channel: UInt64)
    case nativeWindowResized(window: UInt64, width: Int32, height: Int32)
    case contentRectChanged(left: Int32, top: Int32, right: Int32, bottom: Int32)
    case nativeWindowRedrawNeeded(window: UInt64)
    /// A turn the activity handles itself (ADR-0038); the configuration keeps its start values, as Android 15's
    /// asset manager does for such a turn (rotation-test).
    case configurationChanged
    /// A SurfaceView's surface (ADR-0043 c4): the answer to a make, with its window and the buffer's size, and to a
    /// place with another size; the host took it away at the background or the end.
    case surfaceCreated(token: UInt64, window: UInt64, width: Int32, height: Int32)
    case surfaceResized(token: UInt64, window: UInt64, width: Int32, height: Int32)
    case surfaceDestroyed(token: UInt64, window: UInt64)
    /// A window of the guest's own (ADR-0041, gap 4): the answer to a make, with its window and the buffer's size, and
    /// to a place with another size; WINDOW_DESTROYED when the host took it away at the background or the end.
    case windowCreated(token: UInt64, window: UInt64, width: Int32, height: Int32)
    case windowResized(token: UInt64, window: UInt64, width: Int32, height: Int32)
    case windowDestroyed(token: UInt64, window: UInt64)
    /// The answer to an open request (ADR-0062), APKRT_EVENT_OPEN_RESULT.
    case openResult(id: UInt64, outcome: OpenOutcome)
    /// The answer to a pick request (ADR-0057), APKRT_EVENT_PICK_RESULT: Activity's result code (RESULT_OK -1,
    /// RESULT_CANCELED 0) and the number of picks the manifest names.
    case pickResult(id: UInt64, code: Int32, count: UInt64)
    /// The part of the session's view UIKit's keyboard covers, in pixels (ADR-0068 step 2d), APKRT_EVENT_IME_INSETS.
    case imeInsets(bottom: Int32)
    /// The system's back (ADR-0041 decision 10), APKRT_EVENT_BACK: its phase, the gesture's progress in 1/10000 and
    /// its edge, BackEvent's EDGE_LEFT 0 or EDGE_RIGHT 1.
    case back(phase: BackPhase, progress: Int32, edge: Int32)
    /// UIKit's keyboard went away by the user's doing, not the guest's (ADR-0068), APKRT_EVENT_IME_HIDDEN: the guest
    /// handles it as Android's input method hiding itself on BACK.
    case imeHidden
    /// The status bar's and the navigation bar's heights in pixels at a turn (ADR-0041 decision 8, finding 3),
    /// APKRT_EVENT_BAR_INSETS: the bars the guest's floating windows are laid out inside.
    case barInsets(top: Int32, bottom: Int32)
    /// The system trims the app's memory, APKRT_EVENT_TRIM_MEMORY: ComponentCallbacks2's level. Android 15 tells an app
    /// of TRIM_MEMORY_UI_HIDDEN (20) when its UI is no longer visible, which the host posts after onStop; a memory
    /// warning stays `lowMemory`.
    case trimMemory(level: Int32)
    /// The answer to an app installer's question whether it may install apps, APKRT_EVENT_INSTALL_SOURCES_RESULT.
    case installSourcesResult(id: UInt64, granted: Bool)
    /// The answer to an app installer's handed-over APK, APKRT_EVENT_INSTALL_RESULT.
    case installResult(id: UInt64, outcome: InstallOutcome)
    /// The answer to an app's request to remove an app, APKRT_EVENT_UNINSTALL_RESULT (ADR-0077).
    case uninstallResult(id: UInt64, outcome: UninstallOutcome)
}

/// APKRT_EVENT_UNINSTALL_RESULT's outcome (ADR-0077).
public enum UninstallOutcome: UInt64, Sendable, Equatable {
    /// The user confirmed, and the library removed the app.
    case removed = 0
    /// No such app in the library, or its removal failed.
    case failed = 1
    /// The user declined.
    case cancelled = 2
}

/// APKRT_EVENT_INSTALL_RESULT's outcome, as Android's package installer ends.
public enum InstallOutcome: UInt64, Sendable, Equatable {
    /// The app is in the library: RESULT_OK.
    case ready = 0
    /// The APK cannot be installed: RESULT_FIRST_USER.
    case failed = 1
    /// The user declined: RESULT_CANCELED.
    case cancelled = 2
}

/// APKRT_EVENT_BACK's phases (`apkrt_back_phase`): a gesture's start, progress, then commit or cancel; a back without
/// a gesture, the menu's, is a commit alone.
public enum BackPhase: Int32, Sendable, Equatable {
    case start = 1
    case progress
    case commit
    case cancel
}

/// What the guest asks of the host (ADR-0028, decision 4), as the runtime reports it.
public enum SessionRequest: Sendable, Equatable {
    /// `ANativeActivity_finish`.
    case finish
    /// `AWINDOW_FLAG_KEEP_SCREEN_ON` changed.
    case keepScreenOn(Bool)
    /// activity-host cannot start the app, with its reason.
    case startFailed(String)
    /// `ANativeWindow_setFrameRate` changed the rate the window prefers, in frames per second, 0 for none
    /// (ADR-0036, decision 2).
    case frameRate(Float)
    /// The window's opacity changed by Android's `PixelFormat.formatHasAlpha` for the format the app set
    /// (ADR-0030 V5): opaque from NativeActivity's RGB_565 on, so the first report is false. The host's layer
    /// follows it.
    case windowOpaque(Bool)
    /// The guest asks the host to open a URL, write a mail or a message, or share (ADR-0062): the request's ID and the
    /// runtime's checked text.
    case open(id: UInt64, text: String)
    /// The guest no longer wants open request `id`.
    case openCancel(id: UInt64)
    /// The guest asks the user for files, a place to save, a folder or photos (ADR-0057): the request's ID and the
    /// runtime's checked text (PickRequest).
    case pick(id: UInt64, text: String)
    /// The guest no longer wants pick request `id`.
    case pickCancel(id: UInt64)
    /// The guest's input method tells the host about the served editor (ADR-0068 step 2b): the connection's serial and
    /// the request's text (ImeRequest).
    case ime(serial: Int64, text: String)
    /// A back at the task's root (ADR-0041 decision 10), where Android moves the task to the back: the host opens its
    /// menu instead.
    case backAtRoot
    /// The top activity's `setRequestedOrientation`, an ActivityInfo.SCREEN_ORIENTATION_* value: the host turns for
    /// one that forces a side (SessionOrientation(requested:)).
    case orientation(Int32)
    /// A dangerous permission group's request (ADR-0063 decision 2): its ID and "<group>\t<detail>".
    case permission(id: UInt64, text: String)
    /// The guest no longer wants the permission request `id`.
    case permissionCancel(id: UInt64)
    /// A post, cancel or cancel of all of the app's notifications (ADR-0074 decision 2), its text as the runtime
    /// checked it.
    case notification(text: String)
    /// An app installer asks whether it may install apps (APKRT_REQUEST_ASK_INSTALL_SOURCES): "<package>\n<label>".
    case askInstallSources(id: UInt64, text: String)
    /// An app installer hands over an APK (APKRT_REQUEST_CONFIRM_INSTALL): "<path>\n<package>", the guest's path of
    /// the copy below its cache folder and the requesting package.
    case confirmInstall(id: UInt64, text: String)
    /// An app asks to remove an app (APKRT_REQUEST_UNINSTALL, ADR-0077): "<target package>\n<asking package>".
    case uninstall(id: UInt64, text: String)
    /// The starting window's colours the guest framework computed from the starting activity's theme
    /// (APKRT_REQUEST_STARTING_WINDOW, ADR-0076): the splash colour and the icon's background colour, each 0xAARRGGBB,
    /// the latter 0 for none; the host keeps them for the next start.
    case startingWindow(splash: UInt64, iconBackground: UInt64)

    /// APKRT_REQUEST_STARTING_WINDOW's argument: a0, the splash colour, in the low 32 bits, a1, the icon's background
    /// colour, in the high ones.
    public static func startingWindow(packed: UInt64) -> SessionRequest {
        .startingWindow(splash: packed & 0xFFFF_FFFF, iconBackground: packed >> 32)
    }

    /// APKRT_REQUEST_FRAME_RATE's argument, the bits of a float.
    public static func frameRate(bits: UInt64) -> SessionRequest {
        .frameRate(Float(bitPattern: UInt32(truncatingIfNeeded: bits)))
    }

    /// APKRT_REQUEST_ORIENTATION's argument, the value sign-extended.
    public static func orientation(argument: UInt64) -> SessionRequest {
        .orientation(Int32(truncatingIfNeeded: Int64(bitPattern: argument)))
    }

    /// The lifecycle's event for the request, if it has one.
    public var event: Lifecycle.Event? {
        guard case .finish = self else { return nil }
        return .finish
    }
}

/// What the bridge needs of ADR-0028's session and window registry (`apkrt.h`). The app implements it on
/// the runtime, the tests with a fake.
public protocol SessionHost: AnyObject {
    /// `apkrt_session_post`: the event's serial number, from 1, or 0 when the channel is broken for good.
    func post(_ event: SessionEvent) -> UInt32
    /// `apkrt_window_add` with the window's size: its ID, from 1, never reused; its gate starts closed.
    func addWindow(width: Int32, height: Int32) -> UInt64
    /// `apkrt_window_set_gate`.
    func setGate(_ window: UInt64, open: Bool)
    /// `apkrt_window_remove`: the window is gone for good.
    func removeWindow(_ window: UInt64)
    /// `apkrt_set_background`.
    func setBackground(_ on: Bool)
    /// `apkrt_wait_scheduled`: whether every provider reported its GPU work scheduled in time.
    func waitScheduled(seconds: Double) -> Bool
    /// `apkrt_session_wait`: 1 answered, 0 the time is up, -1 the channel broke or the program ended. The bridge
    /// calls it only at the background, on the main thread, within `backgroundAnswerBudget`.
    func waitAnswer(serial: UInt32, seconds: Double) -> Int32
    /// `apkrt_input_add`: a new input channel's ID, from 1, 0 without memory (ADR-0036, decision 1).
    func addInputChannel() -> UInt64
    /// `apkrt_window_set_size`: the window has a new size, which the guest's ANativeWindow answers from then
    /// on (ADR-0038, step c).
    func setWindowSize(_ window: UInt64, width: Int32, height: Int32)
    /// A SurfaceView's window (ADR-0043 c4): `apkrt_window_add` on a layer of its own at `frame`, in the window's
    /// pixels, below the window's layer for a negative z and above it for a positive one; its gate starts closed.
    /// Its ID, or 0 when the host has none for it.
    func addSurface(frame: PixelRect, z: Int32, size: PixelSize, opaque: Bool) -> UInt64
    /// The surface's layer moves to `frame` and `z`.
    func placeSurface(_ window: UInt64, frame: PixelRect, z: Int32)
    /// `apkrt_window_remove` for a surface's window, whose layer goes with it; the window's frame rate stays.
    func removeSurface(_ window: UInt64)
    /// A window of the guest's own (ADR-0041, gap 4): `apkrt_window_add` of the request's buffer size on a layer at its
    /// frame, in ADR-0041's order of windows; its gate starts closed. Its ID, or 0 when the host has none for it.
    func addRequestedWindow(_ request: WindowRequest) -> UInt64
    /// The requested window's layer takes the place's frame, alpha and dim.
    func placeRequestedWindow(_ window: UInt64, _ request: WindowRequest)
    /// `apkrt_window_remove` for requested window `token`'s window, whose layer goes with it.
    func removeRequestedWindow(_ window: UInt64, token: UInt64)
    /// The bridge gives the window up, right before its removal: why (the lifecycle's event, `givingUpReason`) and the
    /// callbacks of that step, for a Debug build's session log (ADR-0043 b5, a black window's first question).
    func windowGivenUp(_ window: UInt64, reason: String, callbacks: [Lifecycle.Callback])
}

extension SessionHost {
    public func windowGivenUp(_ window: UInt64, reason: String, callbacks: [Lifecycle.Callback]) {}
}

/// The guest's window in size compat (ADR-0041, decision 12 b): its frame in the host's window, in pixels, its
/// pixels' scale there, 1 or less, and its own size in pixels.
public struct Letterbox: Sendable, Equatable {
    public var frame: PixelRect
    public var scale: Float
    public var size: PixelSize
    public init(frame: PixelRect, scale: Float, size: PixelSize) {
        self.frame = frame
        self.scale = scale
        self.size = size
    }
}

/// The lifecycle (ADR-0032, decision 7) on ADR-0028's session: each callback becomes an event for
/// activity-host, the window is added and removed around them, and its gate follows the focus.
public struct SessionBridge: Sendable {
    /// A wait for activity-host's answer to the event with `serial`, whose callback the lifecycle awaits.
    public struct Wait: Sendable, Equatable {
        public let serial: UInt32
        public let callback: Lifecycle.Callback
    }

    /// What one event or answer did: the callbacks the guest was sent, and the wait for an answer that the
    /// lifecycle now awaits, which the caller runs off the main thread and ends with `finished`.
    public struct Handled: Sendable, Equatable {
        public var callbacks: [Lifecycle.Callback] = []
        public var wait: Wait?
        /// The waits the bridge ran itself, at the background, for the log.
        public var answers: [Answer] = []
    }

    /// A wait the bridge ran itself: the callback, `apkrt_session_wait`'s result and the seconds it took.
    public struct Answer: Sendable, Equatable {
        public let callback: Lifecycle.Callback
        public let result: Int32
        public let seconds: Double
    }

    /// How long the host waits for the GPU work to be scheduled before the background (ADR-0032,
    /// decision 6).
    public static let scheduledTimeout = 1.0
    /// How long the background waits for the guest's answers, all of them together, before the runtime learns of it
    /// (ADR-0032, decision 6, changed 2026-10-27).
    public static let backgroundAnswerBudget = 1.5

    public private(set) var lifecycle: Lifecycle
    /// The size a new window gets: the window's at its last layout.
    public var windowSize = PixelSize(width: 0, height: 0)
    /// The session turns with the device (ADR-0038, decision 1.3): a layout in another size turns the window.
    public var turns = false
    /// The side the app asked for last with setRequestedOrientation, which the host holds now: a layout in it turns
    /// the window in a session that does not turn too.
    public var requested: SessionOrientation?
    /// The activity handles a size change itself (ADR-0041, decision 12 a): another size in the same orientation
    /// reaches it as a turn does. Without, the guest keeps its size in size compat (decision 12 b).
    public var resizes = false
    /// The bars at the window's next layout (GuestInsets.systemBars), which a turn posts before the configuration;
    /// nil posts none. The start's go through the guest's environment.
    public var systemBars: GuestInsets.Edges?
    /// The guest's size kept in size compat, nil while it has the window's.
    private var keptSize: PixelSize?
    /// The guest has had a window: its size changes only by a change it handles.
    private var guestIsSized = false

    /// The size of the guest's windows: the window's at its last layout, or in size compat the one it kept.
    public var guestSize: PixelSize { keptSize ?? windowSize }

    /// Where the host shows the guest's window in size compat, nil outside it. As Android 15's ActivityRecord
    /// places it: scaled down to fit, never up (updateSizeCompatScale, with Rect.scale's rounding), centered
    /// across and at the top (config_letterboxHorizontalPositionMultiplier 0.5, VerticalPositionMultiplier 0).
    public var letterbox: Letterbox? {
        guard let kept = keptSize, kept.width > 0, kept.height > 0 else { return nil }
        let fits = kept.width <= windowSize.width && kept.height <= windowSize.height
        let scale = fits ? 1 : min(Float(windowSize.width) / Float(kept.width),
                                   Float(windowSize.height) / Float(kept.height))
        let width = Int32(Float(kept.width) * scale + 0.5)
        let height = Int32(Float(kept.height) * scale + 0.5)
        let left = max(0, Int32((Float(windowSize.width - width) * 0.5).rounded(.up)))
        return Letterbox(frame: PixelRect(left: left, top: 0, right: left + width, bottom: height), scale: scale,
                         size: kept)
    }

    /// ActivityInfo's CONFIG_SCREEN_LAYOUT, CONFIG_SCREEN_SIZE and CONFIG_SMALLEST_SCREEN_SIZE.
    static let sizeFlags = 0x0100 | 0x0400 | 0x0800

    /// Whether the launcher activity handles a size change itself (decision 12 a): its `android:configChanges`
    /// hold the screen layout and both sizes; Android adds the sizes itself below targetSdk 13
    /// (ActivityInfo.getRealConfigChanged).
    public static func resizes(configChanges: Int?, targetSdk: Int?) -> Bool {
        var flags = configChanges ?? 0
        if (targetSdk ?? 0) < 13 { flags |= 0x0400 | 0x0800 }
        return flags & sizeFlags == sizeFlags
    }

    /// A post failed: the channel is broken for good, and nothing more reaches the host.
    public private(set) var isBroken = false

    /// The host's window the guest has, 0 for none.
    public private(set) var window: UInt64 = 0
    private var gateIsOpen = false
    /// The input channel of the guest's input queue, once it has one: activity-host destroys the queue
    /// itself before onDestroy. 0 when the runtime had no memory for one.
    public private(set) var inputChannel: UInt64?
    /// The content's rectangle the guest was told last: the whole window, as for an edge-to-edge app.
    private var contentRect: PixelSize?
    /// Whether the runtime was told of the background, when nothing may be shown for the guest (ADR-0062).
    public private(set) var isInBackground = false
    /// The wait that answers the lifecycle's `awaits`; an answer to an earlier one changes nothing.
    private var current: Wait?
    /// The keyboard's cover the guest was told last, in pixels: none at the start, as the guest's ime() inset.
    private var keyboardCover: Int32 = 0

    /// A SurfaceView's surface (ADR-0043 c4), kept as apk-run's headless host keeps one.
    private struct Surface: Sendable, Equatable {
        var token: UInt64
        var window: UInt64
        var size: PixelSize
        /// Its window went with the window's, at the background or the end.
        var taken = false
        /// The guest was told with SURFACE_DESTROYED.
        var announced = false
    }

    /// The surfaces in the order they were made, until the guest removes them.
    private var surfaces: [Surface] = []

    /// A window of the guest's own (ADR-0041, gap 4), kept as apk-run's headless host keeps one.
    private struct RequestedWindow: Sendable, Equatable {
        var token: UInt64
        var window: UInt64
        var size: PixelSize
        /// A sub-window, whose WINDOW_DESTROYED comes before its parent's.
        var sub: Bool
        /// An activity's own window (WindowRequest.isActivityWindow).
        var activity = false
        /// Its window went with the window's, at the background or the end.
        var taken = false
        /// The guest was told with WINDOW_DESTROYED.
        var announced = false
    }

    /// The requested windows in the order they were made, until the guest removes them.
    private var requestedWindows: [RequestedWindow] = []

    /// The activities' own requested windows the guest has, in the order they were made, whose first frame takes the
    /// starting window away as the session's window's does (StartingWindowWatch).
    public var activityWindows: [UInt64] {
        requestedWindows.filter { $0.activity && !$0.taken }.map(\.window)
    }

    /// The windows of the surfaces the guest has, in the order they were made, for the FPS label.
    public var surfaceWindows: [UInt64] {
        surfaces.filter { !$0.taken }.map(\.window)
    }

    public init(lifecycle: Lifecycle) {
        self.lifecycle = lifecycle
    }

    /// The window was laid out in `size` (ADR-0041, decision 12). Before the guest has a window, that is the size
    /// it gets. After, a change the guest handles reaches it: a turn (ADR-0038, decision 1.4) in a session that
    /// turns, another size in one that resizes, as a turn of its window while it has one, as the next window's
    /// size while it has none. A change it does not handle leaves it its size in size compat, which a layout back
    /// at that size ends. A turn between the landscape sides keeps the size and changes nothing for the guest.
    @discardableResult
    public mutating func windowLaidOut(_ size: PixelSize, host: SessionHost) -> Handled {
        guard size != windowSize else { return Handled() }
        let guest = guestSize
        windowSize = size
        guard guestIsSized else { return Handled() }
        guard size != guest else {
            keptSize = nil
            return Handled()
        }
        guard handles(from: guest, to: size) else {
            keptSize = guest
            return Handled()
        }
        keptSize = nil
        guard window != 0 else { return Handled() }
        return handle(.turned, host: host)
    }

    /// A turn swaps the sides, which changes the orientation and the screen size only (ADR-0038); another size
    /// changes the smallest width and the screen layout as well, and the orientation where the longer side moves.
    private func handles(from old: PixelSize, to new: PixelSize) -> Bool {
        if new.width == old.height, new.height == old.width {
            return turns || requested?.fits(width: Double(new.width), height: Double(new.height)) == true
        }
        let turned = (new.width > new.height) != (old.width > old.height)
        return resizes && (turns || !turned)
    }

    /// Hands `event` to the lifecycle and its callbacks to the host.
    @discardableResult
    public mutating func handle(_ event: Lifecycle.Event, host: SessionHost) -> Handled {
        guard !isBroken else { return Handled() }
        switch event {
        case .background where !isInBackground:
            isInBackground = true
            return background(host: host)
        case .foreground where isInBackground:
            isInBackground = false
            host.setBackground(false)
        default:
            break
        }
        return apply(lifecycle.handle(event), host: host, cause: event)
    }

    /// At background, once per trip, even while the lifecycle holds the event back (ADR-0032, decision 6, changed
    /// 2026-10-27): the window goes and the guest hears of it while the runtime still lets its GPU work through, so
    /// that the frames its teardown draws are committed; the bridge waits for its answers on the caller's thread,
    /// `backgroundAnswerBudget` for all of them, and only then the runtime learns of the background and the GPU work
    /// is scheduled before iOS may suspend the process. A wait the budget does not cover goes to the caller as any
    /// other.
    private mutating func background(host: SessionHost) -> Handled {
        var handled = apply(lifecycle.handle(.background), host: host, cause: .background)
        let clock = ContinuousClock()
        let start = clock.now
        while let wait = handled.wait {
            let left = Self.backgroundAnswerBudget - Self.seconds(clock.now - start)
            guard left > 0 else { break }
            let begun = clock.now
            let result = host.waitAnswer(serial: wait.serial, seconds: left)
            handled.answers.append(Answer(callback: wait.callback, result: result,
                                          seconds: Self.seconds(clock.now - begun)))
            guard result != 0 else { break }
            let next = finished(wait, result: result, host: host)
            handled.callbacks += next.callbacks
            handled.wait = next.wait
        }
        host.setBackground(true)
        _ = host.waitScheduled(seconds: Self.scheduledTimeout)
        return handled
    }

    private static func seconds(_ duration: Duration) -> Double {
        Double(duration.components.seconds) + Double(duration.components.attoseconds) / 1e18
    }

    /// A SurfaceView's request as the runtime checked it (ADR-0043 c4), answered as apk-run's headless host answers
    /// it: a make gets a window and SURFACE_CREATED, its gate open while the window's is; a place moves its layer, and
    /// another size gives its window that size, then SURFACE_RESIZED; a remove takes the window away without an event.
    /// A place or remove of a surface the host took away does nothing, but the remove forgets it. After the end every
    /// request is dropped: its answer would never come. Nothing the lifecycle awaits comes of it.
    @discardableResult
    public mutating func surfaceRequested(_ request: SurfaceRequest, host: SessionHost) -> Handled {
        if !isBroken, !lifecycle.hasEnded { answer(request, host: host) }
        return Handled()
    }

    /// A window request as the runtime checked it (ADR-0041, gap 4), answered as apk-run's headless host answers it:
    /// a make gets a window and WINDOW_CREATED, its gate open while the window's is; a place moves its layer, and
    /// another size gives its window that size, then WINDOW_RESIZED; a remove takes the window away without an event.
    /// A place or remove of a window the host took away does nothing, but the remove forgets it. After the end every
    /// request is dropped. Nothing the lifecycle awaits comes of it.
    @discardableResult
    public mutating func windowRequested(_ request: WindowRequest, host: SessionHost) -> Handled {
        if !isBroken, !lifecycle.hasEnded { answer(request, host: host) }
        return Handled()
    }

    private mutating func answer(_ request: WindowRequest, host: SessionHost) {
        let index = requestedWindows.firstIndex { $0.token == request.token }
        let size = request.size
        switch request.operation {
        case .make:
            guard index == nil else { return }  // the runtime refuses it first
            let window = host.addRequestedWindow(request)
            guard window != 0 else { return }
            requestedWindows.append(RequestedWindow(token: request.token, window: window, size: size,
                                                    sub: request.isSubWindow, activity: request.isActivityWindow))
            guard posted(.windowCreated(token: request.token, window: window, width: size.width,
                                        height: size.height), host: host) else { return }
            if gateIsOpen { host.setGate(window, open: true) }
        case .place:
            guard let index, !requestedWindows[index].taken else { return }
            let window = requestedWindows[index].window
            host.placeRequestedWindow(window, request)
            guard size != requestedWindows[index].size else { return }
            requestedWindows[index].size = size
            host.setWindowSize(window, width: size.width, height: size.height)
            _ = posted(.windowResized(token: request.token, window: window, width: size.width, height: size.height),
                       host: host)
        case .remove:
            guard let index else { return }
            if !requestedWindows[index].taken {
                host.removeRequestedWindow(requestedWindows[index].window, token: request.token)
            }
            requestedWindows.remove(at: index)
        }
    }

    /// The answer to open request `id` (ADR-0062), posted unless the channel broke or the session ended.
    @discardableResult
    public mutating func openAnswered(id: UInt64, _ outcome: OpenOutcome, host: SessionHost) -> Handled {
        if !isBroken, !lifecycle.hasEnded { _ = posted(.openResult(id: id, outcome: outcome), host: host) }
        return Handled()
    }

    /// The answer to pick request `id` (ADR-0057): `picked` files named in the manifest, RESULT_OK, or nil for
    /// RESULT_CANCELED; posted unless the channel broke or the session ended.
    @discardableResult
    public mutating func pickAnswered(id: UInt64, picked: Int?, host: SessionHost) -> Handled {
        let event = SessionEvent.pickResult(id: id, code: picked == nil ? 0 : -1, count: UInt64(picked ?? 0))
        if !isBroken, !lifecycle.hasEnded { _ = posted(event, host: host) }
        return Handled()
    }

    /// The answer to install-sources request `id`, posted unless the channel broke or the session ended.
    @discardableResult
    public mutating func installSourcesAnswered(id: UInt64, granted: Bool, host: SessionHost) -> Handled {
        if !isBroken, !lifecycle.hasEnded { _ = posted(.installSourcesResult(id: id, granted: granted), host: host) }
        return Handled()
    }

    /// The answer to install request `id`, posted unless the channel broke or the session ended.
    @discardableResult
    public mutating func installAnswered(id: UInt64, _ outcome: InstallOutcome, host: SessionHost) -> Handled {
        if !isBroken, !lifecycle.hasEnded { _ = posted(.installResult(id: id, outcome: outcome), host: host) }
        return Handled()
    }

    /// The answer to uninstall request `id`, posted unless the channel broke or the session ended.
    @discardableResult
    public mutating func uninstallAnswered(id: UInt64, _ outcome: UninstallOutcome, host: SessionHost) -> Handled {
        if !isBroken, !lifecycle.hasEnded { _ = posted(.uninstallResult(id: id, outcome: outcome), host: host) }
        return Handled()
    }

    /// UIKit's keyboard covers `pixels` of the session's view (ADR-0068 step 2d, KeyboardCover): posted when it
    /// changes, unless the channel broke or the session ended.
    @discardableResult
    public mutating func keyboardCovers(pixels: Int32, host: SessionHost) -> Handled {
        guard pixels != keyboardCover, !isBroken, !lifecycle.hasEnded else { return Handled() }
        keyboardCover = pixels
        _ = posted(.imeInsets(bottom: pixels), host: host)
        return Handled()
    }

    /// The user hid UIKit's keyboard (ADR-0068, APKRT_EVENT_IME_HIDDEN): a tap outside the field, an iPad's dismiss key,
    /// a back the keyboard took; posted each time unless the channel broke or the session ended.
    @discardableResult
    public mutating func keyboardHidden(host: SessionHost) -> Handled {
        guard !isBroken, !lifecycle.hasEnded else { return Handled() }
        _ = posted(.imeHidden, host: host)
        return Handled()
    }

    /// The system's back (ADR-0041 decision 10): posted unless the channel broke or the session ended.
    @discardableResult
    public mutating func back(_ phase: BackPhase, progress: Int32, edge: Int32, host: SessionHost) -> Handled {
        guard !isBroken, !lifecycle.hasEnded else { return Handled() }
        _ = posted(.back(phase: phase, progress: progress, edge: edge), host: host)
        return Handled()
    }

    private mutating func answer(_ request: SurfaceRequest, host: SessionHost) {
        let index = surfaces.firstIndex { $0.token == request.token }
        let size = request.size
        switch request.operation {
        case .make:
            guard index == nil else { return }  // the runtime refuses it first
            let window = host.addSurface(frame: request.frame, z: request.z, size: size, opaque: request.opaque)
            guard window != 0 else { return }
            surfaces.append(Surface(token: request.token, window: window, size: size))
            guard posted(.surfaceCreated(token: request.token, window: window, width: size.width,
                                         height: size.height), host: host) else { return }
            if gateIsOpen { host.setGate(window, open: true) }
        case .place:
            guard let index, !surfaces[index].taken else { return }
            let window = surfaces[index].window
            host.placeSurface(window, frame: request.frame, z: request.z)
            guard size != surfaces[index].size else { return }
            surfaces[index].size = size
            host.setWindowSize(window, width: size.width, height: size.height)
            _ = posted(.surfaceResized(token: request.token, window: window, width: size.width, height: size.height),
                       host: host)
        case .remove:
            guard let index else { return }
            if !surfaces[index].taken { host.removeSurface(surfaces[index].window) }
            surfaces.remove(at: index)
        }
    }

    /// Posts `event`: false once the channel is broken for good.
    private mutating func posted(_ event: SessionEvent, host: SessionHost) -> Bool {
        guard host.post(event) != 0 else {
            isBroken = true
            return false
        }
        return true
    }

    /// The wait ended with `apkrt_session_wait`'s `result`: 1 answered, 0 timed out, -1 the channel broke
    /// or the program ended.
    @discardableResult
    public mutating func finished(_ wait: Wait, result: Int32, host: SessionHost) -> Handled {
        guard !isBroken, wait == current else { return Handled() }
        current = nil
        let event: Lifecycle.Event = result == 1 ? .answered(wait.callback) : .timedOut(wait.callback)
        return apply(lifecycle.handle(event), host: host, cause: event)
    }

    /// Posts `callbacks` in order, with the window's steps around them:
    /// - the gate closes right after the focus's post when the focus goes, so the frame an app draws for
    ///   LOST_FOCUS waits at the closed gate rather than coming after the window is gone; it opens after
    ///   the posts;
    /// - a new window is added before the callbacks that start its life (ADR-0028's foreground order);
    /// - a window that goes is removed before the first callback, the focus's excepted, that comes once its gate is
    ///   closed, since `native_app_glue`'s onPause waits for an app thread that may wait at the closed gate (ADR-0029,
    ///   decision 15): at the background and the menu's Quit before onPause, and for an app that ends with the focus,
    ///   whose onPause comes first with the gate still open as on Android, before onNativeWindowDestroyed (changed by
    ///   App's c6 host fixes);
    /// - the surfaces' windows go with the window's, and at onDestroy those made in the background; their gates follow
    ///   the window's (ADR-0043 c4).
    private mutating func apply(_ callbacks: [Lifecycle.Callback], host: SessionHost,
                                cause: Lifecycle.Event) -> Handled {
        var handled = Handled()
        let givesUpWindow = callbacks.contains(.onNativeWindowDestroyed)
        var gone: UInt64 = 0
        var serial: UInt32 = 0
        for (index, callback) in callbacks.enumerated() {
            if window == 0, callback == .onCreate || callback == .onStart,
               callbacks[index...].contains(.onNativeWindowCreated) {
                window = host.addWindow(width: guestSize.width, height: guestSize.height)
                guestIsSized = true
            }
            if givesUpWindow, window != 0, !gateIsOpen, !Self.isFocus(callback) {
                gone = window
                window = 0
                host.windowGivenUp(gone, reason: Self.givingUpReason(cause), callbacks: callbacks)
                host.removeWindow(gone)
                takeSurfacesAway(host)
            }
            if callback == .onDestroy { takeSurfacesAway(host) }
            for event in sessionEvents(for: callback, gone: gone, host: host) {
                serial = host.post(event)
                guard serial != 0 else {
                    isBroken = true
                    closeGateIfDue(host)
                    return handled
                }
            }
            handled.callbacks.append(callback)
            if Self.isFocus(callback) { closeGateIfDue(host) }
        }
        closeGateIfDue(host)
        if !gateIsOpen, lifecycle.gate == .open, window != 0 {
            gateIsOpen = true
            host.setGate(window, open: true)
            for surface in surfaces where !surface.taken { host.setGate(surface.window, open: true) }
            for requested in requestedWindows where !requested.taken { host.setGate(requested.window, open: true) }
        }
        // The callback the lifecycle awaits is always the last it gave.
        if let callback = lifecycle.awaits, serial != 0 {
            current = Wait(serial: serial, callback: callback)
            handled.wait = current
        }
        return handled
    }

    private mutating func closeGateIfDue(_ host: SessionHost) {
        guard gateIsOpen, lifecycle.gate != .open else { return }
        gateIsOpen = false
        host.setGate(window, open: false)
        for surface in surfaces where !surface.taken { host.setGate(surface.window, open: false) }
        for requested in requestedWindows where !requested.taken { host.setGate(requested.window, open: false) }
    }

    /// Every surface's and every requested window's window goes (apkrt.h's surface and window handlers, apk-run's
    /// take_away): with the window's at the background and the end, and at onDestroy those made in the background,
    /// which hear nothing of it.
    private mutating func takeSurfacesAway(_ host: SessionHost) {
        for index in surfaces.indices where !surfaces[index].taken {
            host.removeSurface(surfaces[index].window)
            surfaces[index].taken = true
        }
        // The requested windows after them, as apk-run takes its windows away (ADR-0041, gap 4).
        for index in requestedWindows.indices where !requestedWindows[index].taken {
            host.removeRequestedWindow(requestedWindows[index].window, token: requestedWindows[index].token)
            requestedWindows[index].taken = true
        }
    }

    /// Why the lifecycle gave the window up, as the session log names it.
    static func givingUpReason(_ event: Lifecycle.Event) -> String {
        switch event {
        case .background: "background"
        case .quit: "quit"
        case .finish: "finish of the last activity"
        case .quitAtRoot: "quitAtRoot"
        default: "\(event)"
        }
    }

    private static func isFocus(_ callback: Lifecycle.Callback) -> Bool {
        if case .onWindowFocusChanged = callback { return true }
        return false
    }

    /// The callback's event, and ADR-0028 d)'s that follow it.
    /// ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN.
    private static let trimMemoryUIHidden: Int32 = 20

    private mutating func sessionEvents(for callback: Lifecycle.Callback, gone: UInt64,
                                        host: SessionHost) -> [SessionEvent] {
        let event = sessionEvent(for: callback, gone: gone)
        switch callback {
        case .onResume where inputChannel == nil:
            let channel = host.addInputChannel()
            inputChannel = channel
            return [event, .inputQueueCreated(channel: channel)]
        case .onNativeWindowCreated:
            let (width, height) = (guestSize.width, guestSize.height)
            var events = [event, .nativeWindowResized(window: window, width: width, height: height)]
            if contentRect != guestSize {
                contentRect = guestSize
                events.append(.contentRectChanged(left: 0, top: 0, right: width, bottom: height))
            }
            return events + [.nativeWindowRedrawNeeded(window: window)]
        case .onNativeWindowDestroyed:
            // After PAUSE and the focus, before the window's and STOP, in the order they were made: where Android 15
            // calls surfaceDestroyed from the stopped window, before onStop (apkrt.h's surface handler).
            var events: [SessionEvent] = []
            for index in surfaces.indices where surfaces[index].taken && !surfaces[index].announced {
                surfaces[index].announced = true
                events.append(.surfaceDestroyed(token: surfaces[index].token, window: surfaces[index].window))
            }
            // Then the requested windows', the sub-windows' first, each kind in the order made (apk-run's
            // post_destroyed).
            for subs in [true, false] {
                for index in requestedWindows.indices where requestedWindows[index].sub == subs
                    && requestedWindows[index].taken && !requestedWindows[index].announced {
                    requestedWindows[index].announced = true
                    events.append(.windowDestroyed(token: requestedWindows[index].token,
                                                   window: requestedWindows[index].window))
                }
            }
            return events + [event]
        case .onStop:
            return [event, .trimMemory(level: Self.trimMemoryUIHidden)]
        case .onConfigurationChanged:
            // The bars first, so that a floating window laid out again at the turn centers between the new ones.
            return (systemBars.map { [.barInsets(top: $0.top, bottom: $0.bottom)] } ?? []) + [event]
        case .onNativeWindowResized:
            host.setWindowSize(window, width: guestSize.width, height: guestSize.height)
            var events = [event]
            if contentRect != guestSize {
                contentRect = guestSize
                events.append(.contentRectChanged(left: 0, top: 0, right: guestSize.width, bottom: guestSize.height))
            }
            return events + [.nativeWindowRedrawNeeded(window: window)]
        default:
            return [event]
        }
    }

    private func sessionEvent(for callback: Lifecycle.Callback, gone: UInt64) -> SessionEvent {
        switch callback {
        case .onCreate: .create
        case .onStart: .start
        case .onResume: .resume
        case .onNativeWindowCreated:
            .nativeWindowCreated(window: window, width: guestSize.width, height: guestSize.height)
        case .onWindowFocusChanged(let focus): .windowFocusChanged(focus)
        case .onPause: .pause
        case .onNativeWindowDestroyed: .nativeWindowDestroyed(window: gone)
        case .onSaveInstanceState: .saveInstanceState
        case .onStop: .stop
        case .onDestroy: .destroy
        case .onLowMemory: .lowMemory
        case .onConfigurationChanged: .configurationChanged
        case .onNativeWindowResized:
            .nativeWindowResized(window: window, width: guestSize.width, height: guestSize.height)
        }
    }
}
