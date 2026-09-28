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
    /// the first onResume (its channel 0 until ADR-0036's exists), and after each new window a resize to
    /// the same size, the content's rectangle when it changed, and redraw needed.
    case inputQueueCreated(channel: UInt64)
    case nativeWindowResized(window: UInt64, width: Int32, height: Int32)
    case contentRectChanged(left: Int32, top: Int32, right: Int32, bottom: Int32)
    case nativeWindowRedrawNeeded(window: UInt64)
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
    /// Whether the runtime was told of the background.
    private var isInBackground = false
    /// The wait that answers the lifecycle's `awaits`; an answer to an earlier one changes nothing.
    private var current: Wait?

    public init(lifecycle: Lifecycle) {
        self.lifecycle = lifecycle
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
    /// - a window that goes is removed before every callback but the focus's, since `native_app_glue`'s
    ///   onPause waits for an app thread that may wait at the closed gate (ADR-0029, decision 15).
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
            if givesUpWindow, window != 0, !Self.isFocus(callback) {
                closeGateIfDue(host)
                gone = window
                window = 0
                host.removeWindow(gone)
            }
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
        }
    }
}
