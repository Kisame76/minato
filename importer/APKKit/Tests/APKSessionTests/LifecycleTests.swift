import Testing

@testable import APKSession

/// ADR-0032, decision 7 and test 5: the callbacks the guest gets for what iOS does, in Android 15's
/// order, and the gate at every point.
@Suite("Lifecycle")
struct LifecycleTests {
    typealias Event = Lifecycle.Event

    /// Android 15's API level: the app targets a current Android.
    static let targetSdk = 35

    /// The callbacks for each event, in turn.
    static func run(_ events: [Event], on lifecycle: inout Lifecycle) -> [[Lifecycle.Callback]] {
        events.map { lifecycle.handle($0) }
    }

    /// Started, active and focused.
    static func running() -> Lifecycle {
        var lifecycle = Lifecycle(targetSdk: Self.targetSdk)
        _ = run([.start, .active], on: &lifecycle)
        return lifecycle
    }

    @Test func startsAndGainsTheFocus() {
        var lifecycle = Lifecycle(targetSdk: Self.targetSdk)
        #expect(lifecycle.gate == .gone)

        #expect(lifecycle.handle(.start) == [.onCreate, .onStart, .onResume, .onNativeWindowCreated])
        #expect(lifecycle.gate == .closed)
        #expect(lifecycle.handle(.active) == [.onWindowFocusChanged(true)])
        #expect(lifecycle.gate == .open)
    }

    @Test func startsWhenAlreadyActive() {
        var lifecycle = Lifecycle(targetSdk: Self.targetSdk)

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
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [.onStop, .onSaveInstanceState])
        #expect(lifecycle.awaits == nil)
        #expect(lifecycle.gate == .gone)
    }

    /// Where onSaveInstanceState falls depends on the app's targetSdk, as in AOSP's ActivityThread:
    /// after onStop from P (28) on, before onStop below it, and before onPause below Honeycomb (11).
    @Test(arguments: [
        (35, [Lifecycle.Callback.onPause, .onNativeWindowDestroyed], [Lifecycle.Callback.onStop, .onSaveInstanceState]),
        (28, [.onPause, .onNativeWindowDestroyed], [.onStop, .onSaveInstanceState]),
        (27, [.onPause, .onNativeWindowDestroyed], [.onSaveInstanceState, .onStop]),
        (11, [.onPause, .onNativeWindowDestroyed], [.onSaveInstanceState, .onStop]),
        (10, [.onSaveInstanceState, .onPause, .onNativeWindowDestroyed], [.onStop]),
    ])
    func savesItsStateWhereItsTargetSdkWantsIt(targetSdk: Int, background: [Lifecycle.Callback], answer: [Lifecycle.Callback]) {
        var lifecycle = Lifecycle(targetSdk: targetSdk)
        _ = Self.run([.start, .active, .inactive], on: &lifecycle)

        #expect(lifecycle.handle(.background) == background)
        #expect(lifecycle.awaits == .onNativeWindowDestroyed)
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == answer)
        #expect(lifecycle.handle(.foreground) == [.onStart, .onResume, .onNativeWindowCreated])
    }

    @Test(arguments: [35, 27, 10])
    func neverSavesTheStateOfAnAppThatEnds(targetSdk: Int) {
        // ActivityThread saves no state once the activity is finishing (mFinished).
        var lifecycle = Lifecycle(targetSdk: targetSdk)
        _ = Self.run([.start, .active], on: &lifecycle)

        #expect(lifecycle.handle(.quit) == [.onWindowFocusChanged(false), .onPause, .onNativeWindowDestroyed])
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [.onStop, .onDestroy])
    }

    @Test func aWaitAlsoEndsAfterTwoSeconds() {
        var lifecycle = Self.running()
        _ = Self.run([.inactive, .background], on: &lifecycle)

        #expect(lifecycle.handle(.timedOut(.onNativeWindowDestroyed)) == [.onStop, .onSaveInstanceState])
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
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [.onStop, .onSaveInstanceState])
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
            == [.onStop, .onSaveInstanceState, .onStart, .onResume, .onNativeWindowCreated, .onWindowFocusChanged(true)])
        #expect(lifecycle.gate == .open)
    }

    @Test func eventsDuringAWaitRunAfterItInTheirOrder() {
        var lifecycle = Self.running()
        _ = Self.run([.menuOpened, .background], on: &lifecycle)

        #expect(Self.run([.memoryWarning, .menuClosed, .finish], on: &lifecycle) == [[], [], []])
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [.onStop, .onSaveInstanceState, .onLowMemory, .onDestroy])
        #expect(lifecycle.awaits == .onDestroy)
        #expect(lifecycle.gate == .gone)
        #expect(lifecycle.handle(.answered(.onDestroy)) == [])
        #expect(lifecycle.hasEnded)
    }

    @Test func ignoresWhatComesBeforeTheStart() {
        // iOS may report the scene's phases before the window is there; the guest starts with the window.
        var lifecycle = Lifecycle(targetSdk: Self.targetSdk)

        #expect(Self.run([.memoryWarning, .background, .foreground], on: &lifecycle) == [[], [], []])
        #expect(lifecycle.awaits == nil)
        #expect(lifecycle.gate == .gone)
        #expect(lifecycle.handle(.start) == [.onCreate, .onStart, .onResume, .onNativeWindowCreated])
    }

    @Test func startsOnlyInTheForeground() {
        // A window that is there while the scene is in the background waits: the guest starts with the
        // foreground, never in the background.
        var lifecycle = Lifecycle(targetSdk: Self.targetSdk)

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
            == [[.onWindowFocusChanged(false)], [.onPause, .onNativeWindowDestroyed], [.onStop, .onSaveInstanceState]])
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
            == [[.onStop, .onSaveInstanceState], [.onStart, .onResume, .onNativeWindowCreated], []])
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
