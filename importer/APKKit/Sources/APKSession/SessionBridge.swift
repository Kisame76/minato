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
    /// The answer to an open request (ADR-0062), APKRT_EVENT_OPEN_RESULT.
    case openResult(id: UInt64, outcome: OpenOutcome)
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

    /// APKRT_REQUEST_FRAME_RATE's argument, the bits of a float.
    public static func frameRate(bits: UInt64) -> SessionRequest {
        .frameRate(Float(bitPattern: UInt32(truncatingIfNeeded: bits)))
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
    }

    /// How long the host waits for the GPU work to be scheduled before the background (ADR-0032,
    /// decision 6).
    public static let scheduledTimeout = 1.0

    public private(set) var lifecycle: Lifecycle
    /// The size a new window gets: the window's at its last layout.
    public var windowSize = PixelSize(width: 0, height: 0)
    /// The session turns with the device (ADR-0038, decision 1.3): a layout in another size turns the window.
    public var turns = false
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

    /// The windows of the surfaces the guest has, in the order they were made, for the FPS label.
    public var surfaceWindows: [UInt64] {
        surfaces.filter { !$0.taken }.map(\.window)
    }

    public init(lifecycle: Lifecycle) {
        self.lifecycle = lifecycle
    }

    /// The window was laid out in `size`: the size the next window gets and, when it changed while the guest
    /// has a window in a session that turns, a turn of that window (ADR-0038, decision 1.4). A turn between
    /// the landscape sides keeps the size and changes nothing for the guest.
    @discardableResult
    public mutating func windowLaidOut(_ size: PixelSize, host: SessionHost) -> Handled {
        guard size != windowSize else { return Handled() }
        windowSize = size
        guard turns, window != 0 else { return Handled() }
        return handle(.turned, host: host)
    }

    /// Hands `event` to the lifecycle and its callbacks to the host.
    @discardableResult
    public mutating func handle(_ event: Lifecycle.Event, host: SessionHost) -> Handled {
        guard !isBroken else { return Handled() }
        switch event {
        // At background, the runtime learns of it first and the GPU work is scheduled before the window
        // goes (ADR-0028, decision 6). Once per trip, even while the lifecycle holds the event back.
        case .background where !isInBackground:
            isInBackground = true
            host.setBackground(true)
            _ = host.waitScheduled(seconds: Self.scheduledTimeout)
        case .foreground where isInBackground:
            isInBackground = false
            host.setBackground(false)
        default:
            break
        }
        return apply(lifecycle.handle(event), host: host)
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

    /// The answer to open request `id` (ADR-0062), posted unless the channel broke or the session ended.
    @discardableResult
    public mutating func openAnswered(id: UInt64, _ outcome: OpenOutcome, host: SessionHost) -> Handled {
        if !isBroken, !lifecycle.hasEnded { _ = posted(.openResult(id: id, outcome: outcome), host: host) }
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
        return apply(lifecycle.handle(event), host: host)
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
    ///   the lead's OK, App's c6 host fixes);
    /// - the surfaces' windows go with the window's, and at onDestroy those made in the background; their gates follow
    ///   the window's (ADR-0043 c4).
    private mutating func apply(_ callbacks: [Lifecycle.Callback], host: SessionHost) -> Handled {
        var handled = Handled()
        let givesUpWindow = callbacks.contains(.onNativeWindowDestroyed)
        var gone: UInt64 = 0
        var serial: UInt32 = 0
        for (index, callback) in callbacks.enumerated() {
            if window == 0, callback == .onCreate || callback == .onStart,
               callbacks[index...].contains(.onNativeWindowCreated) {
                window = host.addWindow(width: windowSize.width, height: windowSize.height)
            }
            if givesUpWindow, window != 0, !gateIsOpen, !Self.isFocus(callback) {
                gone = window
                window = 0
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
    }

    /// Every surface's window goes (apkrt.h's surface handler, apk-run's take_away): with the window's at the
    /// background and the end, and at onDestroy those made in the background, which hear nothing of it.
    private mutating func takeSurfacesAway(_ host: SessionHost) {
        for index in surfaces.indices where !surfaces[index].taken {
            host.removeSurface(surfaces[index].window)
            surfaces[index].taken = true
        }
    }

    private static func isFocus(_ callback: Lifecycle.Callback) -> Bool {
        if case .onWindowFocusChanged = callback { return true }
        return false
    }

    /// The callback's event, and ADR-0028 d)'s that follow it.
    private mutating func sessionEvents(for callback: Lifecycle.Callback, gone: UInt64,
                                        host: SessionHost) -> [SessionEvent] {
        let event = sessionEvent(for: callback, gone: gone)
        switch callback {
        case .onResume where inputChannel == nil:
            let channel = host.addInputChannel()
            inputChannel = channel
            return [event, .inputQueueCreated(channel: channel)]
        case .onNativeWindowCreated:
            let (width, height) = (windowSize.width, windowSize.height)
            var events = [event, .nativeWindowResized(window: window, width: width, height: height)]
            if contentRect != windowSize {
                contentRect = windowSize
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
            return events + [event]
        case .onNativeWindowResized:
            host.setWindowSize(window, width: windowSize.width, height: windowSize.height)
            var events = [event]
            if contentRect != windowSize {
                contentRect = windowSize
                events.append(.contentRectChanged(left: 0, top: 0, right: windowSize.width, bottom: windowSize.height))
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
        case .onNativeWindowCreated: .nativeWindowCreated(window: window, width: windowSize.width, height: windowSize.height)
        case .onWindowFocusChanged(let focus): .windowFocusChanged(focus)
        case .onPause: .pause
        case .onNativeWindowDestroyed: .nativeWindowDestroyed(window: gone)
        case .onSaveInstanceState: .saveInstanceState
        case .onStop: .stop
        case .onDestroy: .destroy
        case .onLowMemory: .lowMemory
        case .onConfigurationChanged: .configurationChanged
        case .onNativeWindowResized: .nativeWindowResized(window: window, width: windowSize.width, height: windowSize.height)
        }
    }
}
