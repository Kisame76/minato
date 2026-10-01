/// The guest's lifecycle in game mode (ADR-0032, decision 7): which Android callbacks the guest gets for
/// what iOS does, in the order Android 15 shows, and the GPU gate at every point (decision 6).
///
/// The host waits for the guest's answer to onNativeWindowDestroyed and onDestroy, at most
/// `answerTimeout`, and reports it as `answered` or `timedOut` with the callback. Events that come
/// meanwhile wait too and run in their order after it, as Android runs them one after the other.
public struct Lifecycle: Sendable, Equatable {
    public enum Event: Sendable, Equatable {
        /// The window exists and the guest is to start.
        case start
        case active
        /// Control Center, the app switcher, a call.
        case inactive
        case background
        case foreground
        case memoryWarning
        case menuOpened
        case menuClosed
        /// Quit in the menu.
        case quit
        /// The guest called ANativeActivity_finish.
        case finish
        /// The window took another size while the session turns (ADR-0038, decision 1.4).
        case turned
        /// The guest's callback that the host waits for returned. The answer to one it no longer waits
        /// for, after a timeout, changes nothing.
        case answered(Callback)
        /// The wait for the callback took `answerTimeout`.
        case timedOut(Callback)
    }

    public enum Callback: Sendable, Equatable {
        case onCreate
        case onStart
        case onResume
        case onNativeWindowCreated
        case onWindowFocusChanged(Bool)
        case onPause
        case onNativeWindowDestroyed
        case onSaveInstanceState
        case onStop
        case onDestroy
        case onLowMemory
        /// A turn the activity handles itself: the configuration, then its window's new size (ADR-0038).
        case onConfigurationChanged
        case onNativeWindowResized
    }

    /// Whether the guest may reach the GPU (decision 6).
    public enum Gate: Sendable, Equatable {
        /// While the app has the focus: the scene is active and the menu closed.
        case open
        /// The window is there, but presents wait.
        case closed
        /// No window: presents fail at once.
        case gone
    }

    /// How long the host waits for the guest's answer to onNativeWindowDestroyed and onDestroy.
    public static let answerTimeout: Duration = .seconds(2)

    private enum Phase: Sendable, Equatable {
        case notStarted
        /// Started or resumed, with its window.
        case resumed
        /// Without a window: from onPause on, stopping or stopped.
        case stopped
        case ended
    }

    private var phase = Phase.notStarted
    /// The window exists (`start`); the guest starts once the scene is in the foreground too.
    private var hasWindow = false
    /// Whether iOS has the scene in the background, also before the start.
    private var isInBackground = false
    private var isActive = false
    private var menuIsOpen = false
    /// What the guest was told last with onWindowFocusChanged.
    private var hasFocus = false
    /// Whether the window now given up ends the app (Quit, finish) rather than going to the background.
    private var isEnding = false
    private var waiting: [Event] = []

    /// The callback whose answer the host waits for.
    public private(set) var awaits: Callback?

    public var hasEnded: Bool { phase == .ended }

    public var gate: Gate {
        guard phase == .resumed else { return .gone }
        return hasFocus ? .open : .closed
    }

    /// The app's targetSdk, which decides where onSaveInstanceState falls, as in AOSP's
    /// `ActivityThread.callActivityOnStop` and `performPauseActivity`: after onStop from P on, before
    /// onStop below P, and before onPause below Honeycomb. An app that ends saves no state.
    private let targetSdk: Int
    private static let honeycomb = 11
    private static let pie = 28

    public init(targetSdk: Int) {
        self.targetSdk = targetSdk
    }

    /// The callbacks for `event`, in order.
    public mutating func handle(_ event: Event) -> [Callback] {
        guard phase != .ended else { return [] }
        switch event {
        case .answered(let callback), .timedOut(let callback):
            guard callback == awaits else { return [] }
        default:
            guard awaits == nil else {
                waiting.append(event)
                return []
            }
        }
        var callbacks = apply(event)
        while awaits == nil, phase != .ended, !waiting.isEmpty {
            callbacks += apply(waiting.removeFirst())
        }
        return callbacks
    }

    private mutating func apply(_ event: Event) -> [Callback] {
        switch event {
        case .start:
            hasWindow = true
            return startIfDue()
        case .active:
            isActive = true
            return updateFocus()
        case .inactive:
            isActive = false
            return updateFocus()
        case .menuOpened:
            menuIsOpen = true
            return updateFocus()
        case .menuClosed:
            menuIsOpen = false
            return updateFocus()
        case .memoryWarning:
            return phase == .notStarted ? [] : [.onLowMemory]
        case .turned:
            // Android 15's order for a turn the activity handles (rotation-test); without a window, the next
            // one has the new size.
            return phase == .resumed ? [.onConfigurationChanged, .onNativeWindowResized] : []
        case .background:
            isInBackground = true
            guard phase == .resumed else { return [] }
            isEnding = false
            return giveUpWindow()
        case .foreground:
            isInBackground = false
            if phase == .notStarted { return startIfDue() }
            guard phase == .stopped else { return [] }
            phase = .resumed
            return [.onStart, .onResume, .onNativeWindowCreated] + updateFocus()
        case .quit, .finish:
            switch phase {
            case .resumed:
                isEnding = true
                return giveUpWindow()
            case .stopped:
                awaits = .onDestroy
                return [.onDestroy]
            case .notStarted:
                phase = .ended
                return []
            case .ended:
                return []
            }
        case .answered, .timedOut:
            let waited = awaits
            awaits = nil
            switch waited {
            case .onNativeWindowDestroyed:
                guard isEnding else {
                    switch targetSdk {
                    case ..<Self.honeycomb: return [.onStop]
                    case ..<Self.pie: return [.onSaveInstanceState, .onStop]
                    default: return [.onStop, .onSaveInstanceState]
                    }
                }
                awaits = .onDestroy
                return [.onStop, .onDestroy]
            case .onDestroy:
                phase = .ended
                return []
            default:
                return []
            }
        }
    }

    /// The guest starts once its window exists and the scene is in the foreground: a window in the
    /// background could reach the GPU.
    private mutating func startIfDue() -> [Callback] {
        guard phase == .notStarted, hasWindow, !isInBackground else { return [] }
        phase = .resumed
        return [.onCreate, .onStart, .onResume, .onNativeWindowCreated] + updateFocus()
    }

    /// onPause and onNativeWindowDestroyed, after the focus is gone. The window goes at once, before
    /// onPause (ADR-0028's order): a render thread held at the closed gate returns to its loop, where
    /// native_app_glue's onPause waits for it.
    private mutating func giveUpWindow() -> [Callback] {
        var callbacks: [Callback] = []
        if hasFocus {
            hasFocus = false
            callbacks.append(.onWindowFocusChanged(false))
        }
        if !isEnding, targetSdk < Self.honeycomb {
            callbacks.append(.onSaveInstanceState)
        }
        phase = .stopped
        awaits = .onNativeWindowDestroyed
        return callbacks + [.onPause, .onNativeWindowDestroyed]
    }

    /// onWindowFocusChanged when the app gains or loses the focus: resumed, active and without the menu.
    private mutating func updateFocus() -> [Callback] {
        let focus = phase == .resumed && isActive && !menuIsOpen
        guard focus != hasFocus else { return [] }
        hasFocus = focus
        return [.onWindowFocusChanged(focus)]
    }
}
