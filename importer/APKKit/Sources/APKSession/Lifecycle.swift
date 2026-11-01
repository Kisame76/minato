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
        /// Quit in the menu that a back at the task's root opened (ADR-0041 decision 10): the background's row, then
        /// the end without onDestroy, as Android may end a task it moved to the back once it is stopped.
        case quitAtRoot
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
    /// Whether the app ends once it is stopped (quitAtRoot).
    private var endsStopped = false
    private var waiting: [Event] = []

    /// The callback whose answer the host waits for.
    public private(set) var awaits: Callback?

    public var hasEnded: Bool { phase == .ended }

    public var gate: Gate {
        guard phase == .resumed else { return .gone }
        return hasFocus ? .open : .closed
    }

    /// Whether the frame clock runs (ADR-0041, decision 2): for a NativeActivity app while its gate is open; for a Java
    /// app from its start to its end while the scene is in the foreground, whatever the gate (changed by the lead's OK,
    /// App's c6 host fixes). Android gives frames to an unfocused or windowless app that asks, and a Java app's
    /// ViewRootImpl holds its main queue's messages, the lifecycle's events among them, behind a traversal that waits
    /// for a frame; its presents still wait at a closed gate.
    public func framesRun(javaApp: Bool) -> Bool {
        guard javaApp else { return gate == .open }
        return phase != .notStarted && phase != .ended && !isInBackground
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
        case .quitAtRoot:
            switch phase {
            case .resumed:
                isEnding = false
                endsStopped = true
                return giveUpWindow()
            case .stopped, .notStarted:
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
                    let stop: [Callback] = switch targetSdk {
                    case ..<Self.honeycomb: [.onStop]
                    case ..<Self.pie: [.onSaveInstanceState, .onStop]
                    default: [.onStop, .onSaveInstanceState]
                    }
                    if endsStopped { awaits = stop.last }
                    return stop
                }
                awaits = .onDestroy
                return [.onStop, .onDestroy]
            case .onDestroy:
                phase = .ended
                return []
            default:
                // The stop that quitAtRoot waited for ends the app.
                if endsStopped, waited != nil { phase = .ended }
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

    /// onPause, the focus's loss if the app still has it, and onNativeWindowDestroyed. An app that ends (Quit,
    /// finish) gets Android 15's order, onPause before the focus goes (activity-test, java-test; changed by the lead's
    /// OK, App's c6 host fixes). The background keeps the focus's loss first, a named departure: iOS takes the focus at
    /// willDeactivate, before it tells of the background. SessionBridge removes the window before the first callback
    /// after its gate closed (ADR-0028's order): a render thread held at the closed gate returns to its loop, where
    /// native_app_glue's onPause waits for it.
    private mutating func giveUpWindow() -> [Callback] {
        let focus: [Callback] = hasFocus ? [.onWindowFocusChanged(false)] : []
        hasFocus = false
        phase = .stopped
        awaits = .onNativeWindowDestroyed
        if isEnding {
            return [.onPause] + focus + [.onNativeWindowDestroyed]
        }
        let save: [Callback] = targetSdk < Self.honeycomb ? [.onSaveInstanceState] : []
        return focus + save + [.onPause, .onNativeWindowDestroyed]
    }

    /// onWindowFocusChanged when the app gains or loses the focus: resumed, active and without the menu.
    private mutating func updateFocus() -> [Callback] {
        let focus = phase == .resumed && isActive && !menuIsOpen
        guard focus != hasFocus else { return [] }
        hasFocus = focus
        return [.onWindowFocusChanged(focus)]
    }
}
