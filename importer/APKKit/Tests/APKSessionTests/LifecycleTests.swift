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
        #expect(lifecycle.handle(.foreground) == [])
        #expect(lifecycle.handle(.menuClosed) == [.onStart, .onResume, .onNativeWindowCreated])
    }

    @Test(arguments: [35, 27, 10])
    func neverSavesTheStateOfAnAppThatEnds(targetSdk: Int) {
        // ActivityThread saves no state once the activity is finishing (mFinished).
        var lifecycle = Lifecycle(targetSdk: targetSdk)
        _ = Self.run([.start, .active], on: &lifecycle)

        #expect(lifecycle.handle(.quit) == [.onPause, .onWindowFocusChanged(false), .onNativeWindowDestroyed])
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

    /// The return leaves the app stopped under the menu, as Android leaves an activity under the keyguard; Continue
    /// starts and resumes it with its window, as the keyguard's dismiss does.
    @Test func comesBackFromTheBackground() {
        var lifecycle = Self.running()
        _ = Self.run([.inactive, .background, .answered(.onNativeWindowDestroyed)], on: &lifecycle)

        #expect(lifecycle.handle(.foreground) == [])
        #expect(lifecycle.gate == .gone)
        #expect(lifecycle.handle(.active) == [])
        #expect(lifecycle.handle(.menuClosed)
            == [.onStart, .onResume, .onNativeWindowCreated, .onWindowFocusChanged(true)])
        #expect(lifecycle.gate == .open)
    }

    /// A menu closed while the app is in the background, as at a Quit that did not end it, lets the return resume.
    @Test func aMenuClosedInTheBackgroundLetsTheReturnResume() {
        var lifecycle = Self.running()
        _ = Self.run([.inactive, .background, .answered(.onNativeWindowDestroyed), .menuClosed], on: &lifecycle)

        #expect(lifecycle.handle(.foreground) == [.onStart, .onResume, .onNativeWindowCreated])
        #expect(lifecycle.handle(.active) == [.onWindowFocusChanged(true)])
    }

    @Test func holdsTheForegroundUntilTheAnswer() {
        // Back before the guest answered onNativeWindowDestroyed: the background finishes first.
        var lifecycle = Self.running()
        _ = Self.run([.inactive, .background], on: &lifecycle)

        #expect(lifecycle.handle(.foreground) == [])
        #expect(lifecycle.handle(.active) == [])
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [.onStop, .onSaveInstanceState])
        #expect(lifecycle.gate == .gone)
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
        // Quit in the menu, or ANativeActivity_finish (an Activity's finish()): onPause before the focus goes, as
        // Android 15 shows it at an app's end (activity-test, java-test; App's c6 host fixes).
        var lifecycle = Self.running()

        #expect(lifecycle.handle(end) == [.onPause, .onWindowFocusChanged(false), .onNativeWindowDestroyed])
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

    @Test func quitInTheMenuEndsThePausedAppWithoutASecondPause() {
        // The menu paused the app and closed the gate, where a render thread may wait; Quit removes the window at once.
        var lifecycle = Self.running()
        _ = lifecycle.handle(.menuOpened)

        #expect(lifecycle.handle(.quit) == [.onNativeWindowDestroyed])
        #expect(lifecycle.gate == .gone)
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [.onStop, .onDestroy])
    }

    @Test func anAppThatFinishesUnderTheMenuEndsWithoutASecondPause() {
        var lifecycle = Self.running()
        _ = lifecycle.handle(.menuOpened)

        #expect(lifecycle.handle(.finish) == [.onNativeWindowDestroyed])
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == [.onStop, .onDestroy])
    }

    /// Quit in the menu that a back at the task's root opened (ADR-0041 decision 10): the background row, as Android
    /// leaves a task in the back stopped with its state saved by targetSdk, then the end without onDestroy, as Android
    /// may end any stopped app.
    @Test(arguments: [35, 27, 10])
    func quitAtTheRootStopsAndEndsWithoutOnDestroy(targetSdk: Int) {
        var lifecycle = Lifecycle(targetSdk: targetSdk)
        let pause: [Lifecycle.Callback] = targetSdk >= 11 ? [.onWindowFocusChanged(false), .onPause]
            : [.onWindowFocusChanged(false), .onSaveInstanceState, .onPause]
        #expect(Self.run([.start, .active, .menuOpened], on: &lifecycle).last == pause)
        let stop: [Lifecycle.Callback] = targetSdk >= 28 ? [.onStop, .onSaveInstanceState]
            : targetSdk >= 11 ? [.onSaveInstanceState, .onStop] : [.onStop]

        #expect(lifecycle.handle(.quitAtRoot) == [.onNativeWindowDestroyed])
        #expect(lifecycle.gate == .gone)
        #expect(lifecycle.handle(.answered(.onNativeWindowDestroyed)) == stop)
        #expect(lifecycle.awaits == stop.last)
        #expect(!lifecycle.hasEnded)
        #expect(lifecycle.handle(.timedOut(stop.last!)) == [])
        #expect(lifecycle.hasEnded)
    }

    @Test func quitAtTheRootInTheBackgroundEndsAtOnce() {
        var lifecycle = Self.running()
        _ = Self.run([.menuOpened, .inactive, .background, .answered(.onNativeWindowDestroyed)], on: &lifecycle)

        #expect(lifecycle.handle(.quitAtRoot) == [])
        #expect(lifecycle.hasEnded)
    }

    @Test func finishesInTheBackground() {
        var lifecycle = Self.running()
        _ = Self.run([.inactive, .background, .answered(.onNativeWindowDestroyed)], on: &lifecycle)

        #expect(lifecycle.handle(.finish) == [.onDestroy])
        #expect(lifecycle.handle(.answered(.onDestroy)) == [])
        #expect(lifecycle.hasEnded)
    }

    /// Minato's menu over the app is as a window over a paused activity on Android: the focus goes, then onPause; its
    /// window stays, behind a closed gate. Continue resumes it, then gives the focus back.
    @Test func theMenuPausesTheAppAndContinueResumesIt() {
        var lifecycle = Self.running()

        #expect(lifecycle.handle(.menuOpened) == [.onWindowFocusChanged(false), .onPause])
        #expect(lifecycle.gate == .closed)
        #expect(!lifecycle.framesRun(javaApp: false))
        #expect(lifecycle.handle(.menuClosed) == [.onResume, .onWindowFocusChanged(true)])
        #expect(lifecycle.gate == .open)
    }

    /// Below Honeycomb every pause saves the state first, as AOSP's performPauseActivity does.
    @Test func theMenuSavesTheStateOfAnOldAppBeforeItsPause() {
        var lifecycle = Lifecycle(targetSdk: 10)
        _ = Self.run([.start, .active], on: &lifecycle)

        #expect(lifecycle.handle(.menuOpened) == [.onWindowFocusChanged(false), .onSaveInstanceState, .onPause])
    }

    /// A menu opened while the scene is inactive pauses all the same; Continue resumes once the scene is active.
    @Test func theMenuPausesWhileInactiveToo() {
        var lifecycle = Self.running()
        _ = lifecycle.handle(.inactive)

        #expect(lifecycle.handle(.menuOpened) == [.onPause])
        #expect(lifecycle.handle(.menuClosed) == [.onResume])
        #expect(lifecycle.handle(.active) == [.onWindowFocusChanged(true)])
    }

    /// The background of a paused app stops it without a second pause, and the menu waits on the return: the app stays
    /// stopped until Continue, as an activity under Android's keyguard.
    @Test func theBackgroundUnderTheMenuStopsAndTheReturnWaitsForContinue() {
        var lifecycle = Self.running()
        _ = lifecycle.handle(.menuOpened)

        #expect(Self.run([.inactive, .background], on: &lifecycle) == [[], [.onNativeWindowDestroyed]])
        #expect(lifecycle.gate == .gone)
        #expect(Self.run([.answered(.onNativeWindowDestroyed), .foreground, .active], on: &lifecycle)
            == [[.onStop, .onSaveInstanceState], [], []])
        #expect(lifecycle.gate == .gone)
        #expect(lifecycle.handle(.menuClosed)
            == [.onStart, .onResume, .onNativeWindowCreated, .onWindowFocusChanged(true)])
        #expect(lifecycle.gate == .open)
    }

    /// Leaving Minato pauses and stops the app as before, and its return shows the menu over the stopped app.
    @Test func theBackgroundOpensTheMenuForTheReturn() {
        var lifecycle = Self.running()
        #expect(!lifecycle.menuIsOpen)

        #expect(Self.run([.inactive, .background], on: &lifecycle)
            == [[.onWindowFocusChanged(false)], [.onPause, .onNativeWindowDestroyed]])
        #expect(lifecycle.menuIsOpen)
        #expect(Self.run([.answered(.onNativeWindowDestroyed), .foreground, .active], on: &lifecycle)
            == [[.onStop, .onSaveInstanceState], [], []])
        #expect(lifecycle.handle(.menuClosed)
            == [.onStart, .onResume, .onNativeWindowCreated, .onWindowFocusChanged(true)])
        #expect(!lifecycle.menuIsOpen)
    }

    /// Quit in the menu that waits on the return ends the stopped app with onDestroy alone, as Android ends a stopped
    /// activity.
    @Test func quitInTheMenuOnTheReturnEndsWithoutAResume() {
        var lifecycle = Self.running()
        _ = Self.run([.inactive, .background, .answered(.onNativeWindowDestroyed), .foreground, .active],
                     on: &lifecycle)

        #expect(lifecycle.handle(.quit) == [.onDestroy])
        #expect(lifecycle.handle(.answered(.onDestroy)) == [])
        #expect(lifecycle.hasEnded)
    }

    /// A Java app's frames run from its start to its end while the scene is in the foreground, whatever the gate
    /// (ADR-0041 decision 2, changed by App's c6 host fixes): its ViewRootImpl holds the main queue's
    /// messages, the lifecycle's among them, behind a traversal that waits for a frame, and Android gives frames to an
    /// unfocused or windowless app that asks.
    @Test func aJavaAppsFramesRunWhileTheSceneIsInTheForeground() {
        var lifecycle = Lifecycle(targetSdk: Self.targetSdk)
        #expect(!lifecycle.framesRun(javaApp: true))

        var steps: [Bool] = []
        for event: Event in [.start, .active, .inactive, .menuOpened, .menuClosed, .active, .background,
                             .answered(.onNativeWindowDestroyed), .foreground, .active, .finish,
                             .answered(.onNativeWindowDestroyed)] {
            _ = lifecycle.handle(event)
            steps.append(lifecycle.framesRun(javaApp: true))
        }
        #expect(steps == [true, true, true, true, true, true, false, false, true, true, true, true])
        _ = lifecycle.handle(.answered(.onDestroy))
        #expect(!lifecycle.framesRun(javaApp: true))
    }

    /// A NativeActivity app's frames run while its gate is open, as before.
    @Test func aNativeActivityAppsFramesRunWhileTheGateIsOpen() {
        var lifecycle = Lifecycle(targetSdk: Self.targetSdk)
        for event: Event in [.start, .active, .inactive, .active, .menuOpened, .menuClosed, .background,
                             .answered(.onNativeWindowDestroyed), .foreground, .active, .finish,
                             .answered(.onNativeWindowDestroyed), .answered(.onDestroy)] {
            _ = lifecycle.handle(event)
            #expect(lifecycle.framesRun(javaApp: false) == (lifecycle.gate == .open))
        }
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

    // ADR-0038, decision 1.4 and test 2: a turn the activity handles itself, in rotation-test's order on
    // Android 15 (fixtures/expected/rotation-test.logcat): the configuration, then the same window resized.

    @Test func aTurnWhileRunningGivesTheConfigurationThenTheResizeAndKeepsTheFocus() {
        var lifecycle = Self.running()

        #expect(lifecycle.handle(.turned) == [.onConfigurationChanged, .onNativeWindowResized])
        #expect(lifecycle.gate == .open)
    }

    @Test func aTurnWithTheMenuOpenKeepsTheGateClosed() {
        var lifecycle = Self.running()
        _ = lifecycle.handle(.menuOpened)

        #expect(lifecycle.handle(.turned) == [.onConfigurationChanged, .onNativeWindowResized])
        #expect(lifecycle.gate == .closed)
    }

    /// Without a window there is nothing to resize: the next window has the new size.
    @Test func aTurnWithoutAWindowGivesNothing() {
        var fresh = Lifecycle(targetSdk: Self.targetSdk)
        #expect(fresh.handle(.turned) == [])

        var lifecycle = Self.running()
        _ = Self.run([.inactive, .background, .answered(.onNativeWindowDestroyed)], on: &lifecycle)
        #expect(lifecycle.handle(.turned) == [])
    }

    /// A turn while the host waits for an answer runs after it, as every event does.
    @Test func aTurnWhileTheHostWaitsComesAfterTheAnswer() {
        var lifecycle = Self.running()
        _ = Self.run([.inactive, .background, .foreground, .menuClosed], on: &lifecycle)
        #expect(lifecycle.awaits == .onNativeWindowDestroyed)

        #expect(lifecycle.handle(.turned) == [])
        let after = lifecycle.handle(.answered(.onNativeWindowDestroyed))
        #expect(after.suffix(2) == [.onConfigurationChanged, .onNativeWindowResized])
    }
}
