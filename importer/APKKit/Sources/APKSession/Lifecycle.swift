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
        /// Paused under Minato's menu, with its window behind the closed gate: as an activity under a dialog of another
        /// task (ADR-0032, decision 7, the menu's pause).
        case paused
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
    /// Whether Minato's menu is over the app: from the menu's opening, and from the background on, so that the
    /// return shows it and the app starts again only with Continue.
    public private(set) var menuIsOpen = false
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
        guard phase == .resumed || phase == .paused else { return .gone }
        return hasFocus ? .open : .closed
    }

    /// Whether the frame clock runs (ADR-0041, decision 2): for a NativeActivity app while its gate is open; for a Java
    /// app from its start to its end while the scene is in the foreground, whatever the gate (changed by the decision,
    /// the c6 host fixes). Android gives frames to an unfocused or windowless app that asks, and a Java app's
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
            // onPause, then the focus's loss, as for a dialog of another task on Android 15 (menu-pause-test).
            return pause() + updateFocus()
        case .menuClosed:
            menuIsOpen = false
            // Continue after the return starts the stopped app, as the keyguard's dismiss does on Android 15.
            guard phase == .stopped, !isInBackground, !isEnding, !endsStopped else { return resume() + updateFocus() }
            phase = .resumed
            return [.onStart, .onResume, .onNativeWindowCreated] + updateFocus()
        case .memoryWarning:
            return phase == .notStarted ? [] : [.onLowMemory]
        case .turned:
            // Android 15's order for a turn the activity handles (rotation-test); without a window, the next
            // one has the new size.
            return phase == .resumed || phase == .paused ? [.onConfigurationChanged, .onNativeWindowResized] : []
        case .background:
            isInBackground = true
            // The return shows the menu over the app (the menu's pause).
            if phase != .notStarted { menuIsOpen = true }
            guard phase == .resumed || phase == .paused else { return [] }
            isEnding = false
            return giveUpWindow()
        case .foreground:
            isInBackground = false
            if phase == .notStarted { return startIfDue() }
            // Under the menu the app stays stopped until Continue, as an activity under Android 15's keyguard.
            guard phase == .stopped, !menuIsOpen else { return [] }
            phase = .resumed
            return [.onStart, .onResume, .onNativeWindowCreated] + updateFocus()
        case .quit, .finish:
            switch phase {
            case .resumed, .paused:
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
            case .resumed, .paused:
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
    /// finish) gets Android 15's order, onPause before the focus goes (activity-test, java-test; changed by
    /// the c6 host fixes). The background keeps the focus's loss first, a named departure: iOS takes the focus at
    /// willDeactivate, before it tells of the background. SessionBridge removes the window before the first callback
    /// after its gate closed (ADR-0028's order): a render thread held at the closed gate returns to its loop, where
    /// native_app_glue's onPause waits for it.
    private mutating func giveUpWindow() -> [Callback] {
        let focus: [Callback] = hasFocus ? [.onWindowFocusChanged(false)] : []
        hasFocus = false
        // A paused app has had its pause, and its state saved where its targetSdk wants it then.
        let wasPaused = phase == .paused
        phase = .stopped
        awaits = .onNativeWindowDestroyed
        if wasPaused { return [.onNativeWindowDestroyed] }
        if isEnding {
            return [.onPause] + focus + [.onNativeWindowDestroyed]
        }
        let save: [Callback] = targetSdk < Self.honeycomb ? [.onSaveInstanceState] : []
        return focus + save + [.onPause, .onNativeWindowDestroyed]
    }

    /// The menu's pause of a resumed app: onPause, after the state's save below Honeycomb, as AOSP's
    /// performPauseActivity does; the window stays.
    private mutating func pause() -> [Callback] {
        guard phase == .resumed else { return [] }
        phase = .paused
        return (targetSdk < Self.honeycomb ? [.onSaveInstanceState] : []) + [.onPause]
    }

    /// Continue resumes an app the menu paused.
    private mutating func resume() -> [Callback] {
        guard phase == .paused else { return [] }
        phase = .resumed
        return [.onResume]
    }

    /// onWindowFocusChanged when the app gains or loses the focus: resumed, active and without the menu.
    private mutating func updateFocus() -> [Callback] {
        let focus = phase == .resumed && isActive && !menuIsOpen
        guard focus != hasFocus else { return [] }
        hasFocus = focus
        return [.onWindowFocusChanged(focus)]
    }
}
