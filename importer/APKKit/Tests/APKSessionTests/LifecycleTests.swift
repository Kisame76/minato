import Testing

@testable import APKSession

/// ADR-0032, decision 7 and test 5: the callbacks the guest gets for what iOS does, in Android 15's
/// order, and the gate at every point.
@Suite("Lifecycle")
struct LifecycleTests {
    typealias Event = Lifecycle.Event

    /// The callbacks for each event, in turn.
    static func run(_ events: [Event], on lifecycle: inout Lifecycle) -> [[Lifecycle.Callback]] {
        events.map { lifecycle.handle($0) }
    }

    /// Started, active and focused.
    static func running() -> Lifecycle {
        var lifecycle = Lifecycle()
        _ = run([.start, .active], on: &lifecycle)
        return lifecycle
    }

    @Test func startsAndGainsTheFocus() {
        var lifecycle = Lifecycle()
        #expect(lifecycle.gate == .gone)

        #expect(lifecycle.handle(.start) == [.onCreate, .onStart, .onResume, .onNativeWindowCreated])
        #expect(lifecycle.gate == .closed)
        #expect(lifecycle.handle(.active) == [.onWindowFocusChanged(true)])
        #expect(lifecycle.gate == .open)
    }

    @Test func startsWhenAlreadyActive() {
        var lifecycle = Lifecycle()

        #expect(lifecycle.handle(.active) == [])
        #expect(lifecycle.handle(.start) == [.onCreate, .onStart, .onResume, .onNativeWindowCreated, .onWindowFocusChanged(true)])
        #expect(lifecycle.gate == .open)
    }

    @Test func losesAndRegainsTheFocusWhileInactive() {
        // Control Center, the app switcher, a call.
        var lifecycle = Self.running()

        #expect(lifecycle.handle(.inactive) == [.onWindowFocusChanged(false)])
        #expect(lifecycle.gate == .closed)
        #expect(lifecycle.handle(.active) == [.onWindowFocusChanged(true)])
        #expect(lifecycle.gate == .open)
    }

    @Test func givesUpTheWindowInTheBackgroundBeforeOnPause() {
        // ADR-0028's order: the window goes first, so a render thread held at the closed gate returns to
        // its loop, where native_app_glue's onPause waits for it.
        var lifecycle = Self.running()
        _ = lifecycle.handle(.inactive)

        #expect(lifecycle.handle(.background) == [.onPause, .onNativeWindowDestroyed])
        #expect(lifecycle.awaits == .onNativeWindowDestroyed)
        #expect(lifecycle.gate == .gone)
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [.onSaveInstanceState, .onStop])
        #expect(lifecycle.awaits == nil)
        #expect(lifecycle.gate == .gone)
    }

    @Test func aWaitAlsoEndsAfterTwoSeconds() {
        var lifecycle = Self.running()
        _ = Self.run([.inactive, .background], on: &lifecycle)

        #expect(lifecycle.handle(.timedOut(.onNativeWindowDestroyed)) == [.onSaveInstanceState, .onStop])
        #expect(lifecycle.gate == .gone)
        #expect(Lifecycle.answerTimeout == .seconds(2))
    }

    @Test func aLateAnswerChangesNothing() {
        // The answer to onNativeWindowDestroyed after its wait timed out: onDestroy is still due.
        var lifecycle = Self.running()
        _ = Self.run([.quit, .timedOut(.onNativeWindowDestroyed)], on: &lifecycle)

        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [])
        #expect(lifecycle.awaits == .onDestroy)
        #expect(!lifecycle.hasEnded)
        #expect(lifecycle.handle(.answered(.onDestroy)) == [])
        #expect(lifecycle.hasEnded)
    }

    @Test func anAnswerToAnotherCallbackChangesNothing() {
        var lifecycle = Self.running()

        #expect(lifecycle.handle(.answered(.onDestroy)) == [])
        _ = Self.run([.inactive, .background], on: &lifecycle)
        #expect(Self.run([.answered(.onDestroy), .timedOut(.onDestroy)], on: &lifecycle) == [[], []])
        #expect(lifecycle.awaits == .onNativeWindowDestroyed)
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [.onSaveInstanceState, .onStop])
    }

    @Test func comesBackFromTheBackground() {
        var lifecycle = Self.running()
        _ = Self.run([.inactive, .background, .answered(.onNativeWindowDestroyed)], on: &lifecycle)

        #expect(lifecycle.handle(.foreground) == [.onStart, .onResume, .onNativeWindowCreated])
        #expect(lifecycle.gate == .closed)
        #expect(lifecycle.handle(.active) == [.onWindowFocusChanged(true)])
        #expect(lifecycle.gate == .open)
    }

    @Test func holdsTheForegroundUntilTheAnswer() {
        // Back before the guest answered onNativeWindowDestroyed: the background finishes first.
        var lifecycle = Self.running()
        _ = Self.run([.inactive, .background], on: &lifecycle)

        #expect(lifecycle.handle(.foreground) == [])
        #expect(lifecycle.handle(.active) == [])
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed))
            == [.onSaveInstanceState, .onStop, .onStart, .onResume, .onNativeWindowCreated, .onWindowFocusChanged(true)])
        #expect(lifecycle.gate == .open)
    }

    @Test func eventsDuringAWaitRunAfterItInTheirOrder() {
        var lifecycle = Self.running()
        _ = Self.run([.menuOpened, .background], on: &lifecycle)

        #expect(Self.run([.memoryWarning, .menuClosed, .finish], on: &lifecycle) == [[], [], []])
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [.onSaveInstanceState, .onStop, .onLowMemory, .onDestroy])
        #expect(lifecycle.awaits == .onDestroy)
        #expect(lifecycle.gate == .gone)
        #expect(lifecycle.handle(.answered(.onDestroy)) == [])
        #expect(lifecycle.hasEnded)
    }

    @Test func ignoresWhatComesBeforeTheStart() {
        // iOS may report the scene's phases before the window is there; the guest starts with the window.
        var lifecycle = Lifecycle()

        #expect(Self.run([.memoryWarning, .background, .foreground], on: &lifecycle) == [[], [], []])
        #expect(lifecycle.awaits == nil)
        #expect(lifecycle.gate == .gone)
        #expect(lifecycle.handle(.start) == [.onCreate, .onStart, .onResume, .onNativeWindowCreated])
    }

    @Test func startsOnlyInTheForeground() {
        // A window that is there while the scene is in the background waits: the guest starts with the
        // foreground, never in the background.
        var lifecycle = Lifecycle()

        #expect(Self.run([.inactive, .background, .start], on: &lifecycle) == [[], [], []])
        #expect(lifecycle.gate == .gone)
        #expect(lifecycle.handle(.foreground) == [.onCreate, .onStart, .onResume, .onNativeWindowCreated])
        #expect(lifecycle.gate == .closed)
        #expect(lifecycle.handle(.active) == [.onWindowFocusChanged(true)])
        #expect(lifecycle.gate == .open)
    }

    @Test func ignoresABackgroundOrForegroundItIsAlreadyIn() {
        var lifecycle = Self.running()

        #expect(lifecycle.handle(.foreground) == [])
        #expect(Self.run([.inactive, .background, .answered(.onNativeWindowDestroyed)], on: &lifecycle)
            == [[.onWindowFocusChanged(false)], [.onPause, .onNativeWindowDestroyed], [.onSaveInstanceState, .onStop]])
        #expect(lifecycle.handle(.background) == [])
        #expect(lifecycle.awaits == nil)
        #expect(lifecycle.gate == .gone)
    }

    @Test func passesOnEveryMemoryWarning() {
        var lifecycle = Self.running()

        #expect(Self.run([.memoryWarning, .memoryWarning], on: &lifecycle) == [[.onLowMemory], [.onLowMemory]])
        #expect(lifecycle.gate == .open)
    }

    @Test(arguments: [Event.quit, .finish])
    func endsWithoutSavingItsState(end: Event) {
        // Quit in the menu, or ANativeActivity_finish.
        var lifecycle = Self.running()

        #expect(lifecycle.handle(end) == [.onWindowFocusChanged(false), .onPause, .onNativeWindowDestroyed])
        #expect(lifecycle.gate == .gone)
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [.onStop, .onDestroy])
        #expect(lifecycle.awaits == .onDestroy)
        #expect(lifecycle.gate == .gone)
        #expect(!lifecycle.hasEnded)
        #expect(lifecycle.handle(.answered(.onDestroy)) == [])
        #expect(lifecycle.hasEnded)
    }

    @Test func quitsWhileInactive() {
        var lifecycle = Self.running()
        _ = lifecycle.handle(.inactive)

        #expect(lifecycle.handle(.quit) == [.onPause, .onNativeWindowDestroyed])
        #expect(lifecycle.gate == .gone)
        #expect(lifecycle.handle(.timedOut(.onNativeWindowDestroyed)) == [.onStop, .onDestroy])
        #expect(lifecycle.handle(.timedOut(.onDestroy)) == [])
        #expect(lifecycle.hasEnded)
    }

    @Test func quitInTheMenuLetsGoOfTheGateBeforeOnPause() {
        // The menu closed the gate, where a render thread may wait; Quit removes the window at once.
        var lifecycle = Self.running()
        _ = lifecycle.handle(.menuOpened)

        #expect(lifecycle.handle(.quit) == [.onPause, .onNativeWindowDestroyed])
        #expect(lifecycle.gate == .gone)
    }

    @Test func finishesInTheBackground() {
        var lifecycle = Self.running()
        _ = Self.run([.inactive, .background, .answered(.onNativeWindowDestroyed)], on: &lifecycle)

        #expect(lifecycle.handle(.finish) == [.onDestroy])
        #expect(lifecycle.handle(.answered(.onDestroy)) == [])
        #expect(lifecycle.hasEnded)
    }

    @Test func theMenuTakesTheFocusAndKeepsItOverTheBackground() {
        var lifecycle = Self.running()

        #expect(lifecycle.handle(.menuOpened) == [.onWindowFocusChanged(false)])
        #expect(lifecycle.gate == .closed)
        #expect(Self.run([.inactive, .background], on: &lifecycle) == [[], [.onPause, .onNativeWindowDestroyed]])
        #expect(lifecycle.gate == .gone)
        #expect(Self.run([.answered(.onNativeWindowDestroyed), .foreground, .active], on: &lifecycle)
            == [[.onSaveInstanceState, .onStop], [.onStart, .onResume, .onNativeWindowCreated], []])
        #expect(lifecycle.gate == .closed)
        #expect(lifecycle.handle(.menuClosed) == [.onWindowFocusChanged(true)])
        #expect(lifecycle.gate == .open)
    }

    @Test func nothingQueuedRunsAfterOnDestroy() {
        var lifecycle = Self.running()
        _ = Self.run([.quit, .memoryWarning], on: &lifecycle)

        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [.onStop, .onDestroy])
        _ = lifecycle.handle(.memoryWarning)
        #expect(lifecycle.handle(.answered(.onDestroy)) == [])
        #expect(lifecycle.hasEnded)
    }

    @Test func doesNothingAfterTheEnd() {
        var lifecycle = Self.running()
        _ = Self.run([.quit, .answered(.onNativeWindowDestroyed), .answered(.onDestroy)], on: &lifecycle)

        #expect(Self.run([.active, .memoryWarning, .background, .quit], on: &lifecycle) == [[], [], [], []])
        #expect(lifecycle.gate == .gone)
    }
}
