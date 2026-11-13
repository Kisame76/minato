import Foundation
import APKLibrary
import Testing

@testable import APKSession

/// ADR-0032, step c: the lifecycle's callbacks as ADR-0028's session events, the window registry and the
/// gate, in the order ADR-0028 (decision 6) and ADR-0029 (decision 15) ask for.
@Suite("SessionBridge")
struct SessionBridgeTests {
    /// What the bridge asked of the host, in order.
    enum Call: Equatable {
        case post(SessionEvent)
        case addWindow(width: Int32, height: Int32)
        case setGate(UInt64, open: Bool)
        case removeWindow(UInt64)
        case setBackground(Bool)
        case waitScheduled(Double)
        case waitAnswer(UInt32)
        case addInputChannel
        case setWindowSize(UInt64, width: Int32, height: Int32)
        case addSurface(frame: PixelRect, z: Int32, size: PixelSize, opaque: Bool)
        case placeSurface(UInt64, frame: PixelRect, z: Int32)
        case removeSurface(UInt64)
        case addRequestedWindow(token: UInt64)
        case placeRequestedWindow(UInt64, frame: PixelRect, alpha: Float, dim: Float)
        case removeRequestedWindow(UInt64)
    }

    /// A session that records every call; window IDs and serial numbers count from 1, as the runtime's.
    final class FakeHost: SessionHost {
        var calls: [Call] = []
        private var nextWindow: UInt64 = 1
        private var nextSerial: UInt32 = 1
        /// The post with this serial and every later one fails: the channel broke.
        var breaksAt: UInt32?

        func post(_ event: SessionEvent) -> UInt32 {
            calls.append(.post(event))
            if let breaksAt, nextSerial >= breaksAt { return 0 }
            defer { nextSerial += 1 }
            return nextSerial
        }

        func addWindow(width: Int32, height: Int32) -> UInt64 {
            calls.append(.addWindow(width: width, height: height))
            defer { nextWindow += 1 }
            return nextWindow
        }

        func setGate(_ window: UInt64, open: Bool) { calls.append(.setGate(window, open: open)) }
        func removeWindow(_ window: UInt64) { calls.append(.removeWindow(window)) }
        /// The windows removed until told, and those told, apart from `calls`.
        var removedUntilTold: [UInt64] = []
        var told: [UInt64] = []
        func removeWindowUntilTold(_ window: UInt64) {
            calls.append(.removeWindow(window))
            removedUntilTold.append(window)
        }
        func windowTold(_ window: UInt64) { told.append(window) }
        /// The surfaces' windows removed until told, apart from `calls` (which records them as removeSurface).
        var surfacesUntilTold: [UInt64] = []
        func removeSurfaceUntilTold(_ window: UInt64) {
            calls.append(.removeSurface(window))
            surfacesUntilTold.append(window)
        }
        func setBackground(_ on: Bool) { calls.append(.setBackground(on)) }

        func setWindowSize(_ window: UInt64, width: Int32, height: Int32) {
            calls.append(.setWindowSize(window, width: width, height: height))
        }

        /// The host has no window for a surface: addSurface answers 0.
        var refusesSurfaces = false

        /// A surface's window counts with the session's windows, as in the runtime's registry.
        func addSurface(frame: PixelRect, z: Int32, size: PixelSize, opaque: Bool) -> UInt64 {
            calls.append(.addSurface(frame: frame, z: z, size: size, opaque: opaque))
            guard !refusesSurfaces else { return 0 }
            defer { nextWindow += 1 }
            return nextWindow
        }

        func placeSurface(_ window: UInt64, frame: PixelRect, z: Int32) {
            calls.append(.placeSurface(window, frame: frame, z: z))
        }

        func removeSurface(_ window: UInt64) { calls.append(.removeSurface(window)) }

        /// A requested window counts with the session's windows too.
        func addRequestedWindow(_ request: WindowRequest) -> UInt64 {
            calls.append(.addRequestedWindow(token: request.token))
            defer { nextWindow += 1 }
            return nextWindow
        }

        func placeRequestedWindow(_ window: UInt64, _ request: WindowRequest) {
            calls.append(.placeRequestedWindow(window, frame: request.frame, alpha: request.alpha, dim: request.dim))
        }

        func removeRequestedWindow(_ window: UInt64, token: UInt64) { calls.append(.removeRequestedWindow(window)) }

        func waitScheduled(seconds: Double) -> Bool {
            calls.append(.waitScheduled(seconds))
            return true
        }

        /// The results the waits for answers get, in order; 0 (the time is up) once they are used up.
        var answers: [Int32] = []
        /// The time each wait for an answer was given.
        var answerSeconds: [Double] = []

        func waitAnswer(serial: UInt32, seconds: Double) -> Int32 {
            calls.append(.waitAnswer(serial))
            answerSeconds.append(seconds)
            return answers.isEmpty ? 0 : answers.removeFirst()
        }

        /// The runtime's IDs count from 1; 7 tells the channel from a window or a serial.
        func addInputChannel() -> UInt64 {
            calls.append(.addInputChannel)
            return 7
        }

        /// The windows the bridge gave up, why and with which callbacks (a Debug session log's line), outside `calls`.
        var givenUp: [(window: UInt64, reason: String, callbacks: [Lifecycle.Callback])] = []

        func windowGivenUp(_ window: UInt64, reason: String, callbacks: [Lifecycle.Callback]) {
            givenUp.append((window, reason, callbacks))
        }

        /// The calls since the last look.
        func take() -> [Call] {
            defer { calls = [] }
            return calls
        }
    }

    static let width: Int32 = 1290
    static let height: Int32 = 2796

    static func bridge() -> SessionBridge {
        var bridge = SessionBridge(lifecycle: Lifecycle(targetSdk: 35))
        bridge.windowSize = PixelSize(width: width, height: height)
        return bridge
    }

    /// ADR-0028 d)'s events around a new window, as Android 15 sends them: created, then resized once with
    /// the same size, the content's rectangle (the whole window) at the first only, then redraw needed.
    static func shown(_ window: UInt64, first: Bool) -> [Call] {
        [.post(.nativeWindowCreated(window: window, width: width, height: height)),
         .post(.nativeWindowResized(window: window, width: width, height: height))]
            + (first ? [.post(.contentRectChanged(left: 0, top: 0, right: width, bottom: height))] : [])
            + [.post(.nativeWindowRedrawNeeded(window: window))]
    }

    /// Started, active and focused, with window 1.
    static func running(_ host: FakeHost) -> SessionBridge {
        var bridge = bridge()
        bridge.handle(.start, host: host)
        bridge.handle(.active, host: host)
        _ = host.take()
        return bridge
    }

    /// The host hears why the window goes (ADR-0043 b5, a black window's first question): the lifecycle's event that
    /// gave it up, with that batch's callbacks.
    @Test(arguments: [(Lifecycle.Event.background, "background"), (.quit, "quit"),
                      (.finish, "finish of the last activity"), (.quitAtRoot, "quitAtRoot")])
    func theHostHearsWhyTheWindowGoes(event: Lifecycle.Event, reason: String) {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(event, host: host)
        #expect(host.givenUp.map(\.window) == [1])
        #expect(host.givenUp.map(\.reason) == [reason])
        #expect(host.givenUp.first?.callbacks.last == .onNativeWindowDestroyed)
    }

    /// A turn keeps the window: its size changes, and no window goes.
    @Test func aTurnGivesNoWindowUp() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.turns = true
        bridge.windowLaidOut(PixelSize(width: Self.height, height: Self.width), host: host)
        #expect(host.take().contains(.setWindowSize(1, width: Self.height, height: Self.width)))
        #expect(host.givenUp.isEmpty)
    }

    @Test func startAddsTheWindowAndOpensTheGateWithTheFocus() {
        let host = FakeHost()
        var bridge = Self.bridge()
        #expect(bridge.inputChannel == nil)

        bridge.handle(.start, host: host)
        #expect(bridge.inputChannel == 7)
        #expect(host.take() == [
            .addWindow(width: Self.width, height: Self.height),
            .post(.create), .post(.start), .addInputChannel, .post(.resume), .post(.inputQueueCreated(channel: 7)),
        ] + Self.shown(1, first: true))
        bridge.handle(.active, host: host)
        #expect(host.take() == [.post(.windowFocusChanged(true)), .setGate(1, open: true)])
    }

    /// The FPS label samples the presents of the window the guest has (ADR-0032, decision 9).
    @Test func theWindowIsTheOneTheGuestHas() {
        let host = FakeHost()
        var bridge = Self.bridge()
        #expect(bridge.window == 0)

        bridge.handle(.start, host: host)
        #expect(bridge.window == 1)
        bridge.handle(.inactive, host: host)
        bridge.handle(.background, host: host)
        #expect(bridge.window == 0)
    }

    @Test func startWhenAlreadyActiveOpensTheGateAfterTheEvents() {
        let host = FakeHost()
        var bridge = Self.bridge()

        bridge.handle(.active, host: host)
        #expect(host.take() == [])
        bridge.handle(.start, host: host)
        #expect(host.take() == [
            .addWindow(width: Self.width, height: Self.height),
            .post(.create), .post(.start), .addInputChannel, .post(.resume), .post(.inputQueueCreated(channel: 7)),
        ] + Self.shown(1, first: true) + [.post(.windowFocusChanged(true)), .setGate(1, open: true)])
    }

    /// The focus's post comes first: an animating app draws its LOST_FOCUS frame, which then waits at the
    /// closed gate rather than coming after the window is gone (the graphics host's finding, ADR-0028 d).
    @Test func theFocusGoesBeforeTheGateCloses() {
        let host = FakeHost()
        var bridge = Self.running(host)

        bridge.handle(.inactive, host: host)
        #expect(host.take() == [.post(.windowFocusChanged(false)), .setGate(1, open: false)])
        bridge.handle(.menuOpened, host: host)
        #expect(host.take() == [.post(.pause)])
        bridge.handle(.active, host: host)
        #expect(host.take() == [])
        bridge.handle(.menuClosed, host: host)
        #expect(host.take() == [.post(.resume), .post(.windowFocusChanged(true)), .setGate(1, open: true)])
    }

    /// The program ended while the focus went: the gate closes all the same.
    @Test func aFailedFocusPostStillClosesTheGate() {
        let host = FakeHost()
        var bridge = Self.running(host)
        host.breaksAt = 10

        bridge.handle(.inactive, host: host)
        #expect(host.take() == [.post(.windowFocusChanged(false)), .setGate(1, open: false)])
        #expect(bridge.isBroken)
    }

    /// At the background the window goes before onPause, and the guest hears of it while the runtime still lets its
    /// GPU work through, so that its teardown's last frames are committed; only once it answered, or the budget is
    /// spent, does the runtime learn of the background and the GPU work get scheduled (ADR-0032 d6, changed
    /// 2026-10-27).
    @Test func backgroundTellsTheGuestBeforeTheRuntime() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        _ = host.take()
        host.answers = [1]

        let handled = bridge.handle(.background, host: host)
        #expect(host.take() == [
            .removeWindow(1), .post(.pause), .post(.nativeWindowDestroyed(window: 1)), .waitAnswer(12),
            .post(.stop), .post(.trimMemory(level: 20)), .post(.saveInstanceState),
            .setBackground(true), .waitScheduled(SessionBridge.scheduledTimeout),
        ])
        #expect(handled.callbacks == [.onPause, .onNativeWindowDestroyed, .onStop, .onSaveInstanceState])
        #expect(handled.wait == nil)
        #expect(handled.answers.map(\.callback) == [.onNativeWindowDestroyed])
        #expect(handled.answers.map(\.result) == [1])
        let seconds = try #require(host.answerSeconds.first)
        #expect(seconds > SessionBridge.backgroundAnswerBudget - 0.1 && seconds <= SessionBridge.backgroundAnswerBudget)
    }

    /// The window goes until its destroy callback has answered: its swaps succeed without a frame until then, as
    /// Android's BufferQueue takes frames until surfaceDestroyed has returned, and fail after it.
    @Test func backgroundRemovesTheWindowUntilItsDestroyCallbackAnswered() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        host.answers = [1]
        bridge.handle(.background, host: host)
        #expect(host.removedUntilTold == [1])
        #expect(host.told == [1])
        #expect(bridge.untoldWindow == 0)
    }

    /// An answer that comes after the budget tells the window when it comes; none before it.
    @Test func aDestroyAnswerAfterTheBudgetTellsTheWindowWhenItComes() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        let handled = bridge.handle(.background, host: host)
        #expect(host.removedUntilTold == [1])
        #expect(host.told.isEmpty)
        #expect(bridge.untoldWindow == 1)
        let wait = try #require(handled.wait)
        #expect(wait.callback == .onNativeWindowDestroyed)
        _ = bridge.finished(wait, result: 1, host: host)
        #expect(host.told == [1])
        #expect(bridge.untoldWindow == 0)
    }

    /// Each callback comes with the serial of its first event, so that the host can time the guest's answer to the
    /// pause and the stop of a trip to the background (BackgroundTrace); a wait's answer carries when it came.
    @Test func theCallbacksComeWithTheSerialsOfTheirFirstEvents() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        _ = host.take()
        host.answers = [1]
        let before = DispatchTime.now().uptimeNanoseconds

        let handled = bridge.handle(.background, host: host)
        let after = DispatchTime.now().uptimeNanoseconds
        #expect(handled.callbacks == [.onPause, .onNativeWindowDestroyed, .onStop, .onSaveInstanceState])
        #expect(handled.serials == [11, 12, 13, 15])
        let answer = try #require(handled.answers.first)
        #expect(answer.endedAt >= before && answer.endedAt <= after)
    }

    /// A guest that does not answer within the budget does not hold the background: the runtime learns of it, and the
    /// wait goes on as any other, whose answer then brings onStop.
    @Test func aBackgroundWaitThatRunsOutGoesOnAfterTheRuntimeLearnsOfIt() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        _ = host.take()

        let handled = bridge.handle(.background, host: host)
        #expect(host.take() == [
            .removeWindow(1), .post(.pause), .post(.nativeWindowDestroyed(window: 1)), .waitAnswer(12),
            .setBackground(true), .waitScheduled(SessionBridge.scheduledTimeout),
        ])
        #expect(handled.callbacks == [.onPause, .onNativeWindowDestroyed])
        #expect(handled.answers.map(\.result) == [0])
        let wait = try #require(handled.wait)
        #expect(wait == SessionBridge.Wait(serial: 12, callback: .onNativeWindowDestroyed))

        #expect(bridge.finished(wait, result: 1, host: host).wait == nil)
        #expect(host.take() == [.post(.stop), .post(.trimMemory(level: 20)), .post(.saveInstanceState)])
    }

    /// A channel that broke or a program that ended while the background waited goes on at once, as a wait off the
    /// main thread does.
    @Test func aBackgroundWaitThatBreaksGoesOnBeforeTheRuntimeLearnsOfIt() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        _ = host.take()
        host.answers = [-1]

        let handled = bridge.handle(.background, host: host)
        #expect(host.take() == [
            .removeWindow(1), .post(.pause), .post(.nativeWindowDestroyed(window: 1)), .waitAnswer(12),
            .post(.stop), .post(.trimMemory(level: 20)), .post(.saveInstanceState),
            .setBackground(true), .waitScheduled(SessionBridge.scheduledTimeout),
        ])
        #expect(handled.wait == nil)
        #expect(host.answerSeconds.count == 1)
    }

    @Test func foregroundAddsANewWindowAndOpensItsGateOnceActive() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        let wait = bridge.handle(.background, host: host).wait
        _ = bridge.finished(wait!, result: 1, host: host).wait
        _ = host.take()

        bridge.handle(.foreground, host: host)
        #expect(host.take() == [.setBackground(false)])
        // Minato's menu waits over the stopped app, which starts again with Continue, as after Android's keyguard.
        bridge.handle(.active, host: host)
        #expect(host.take() == [])
        bridge.handle(.menuClosed, host: host)
        #expect(host.take() == [
            .addWindow(width: Self.width, height: Self.height), .post(.start), .post(.resume),
        ] + Self.shown(2, first: false) + [.post(.windowFocusChanged(true)), .setGate(2, open: true)])
    }

    @Test func foregroundBeforeTheAnswerComesAfterIt() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        let wait = bridge.handle(.background, host: host).wait
        _ = host.take()

        #expect(bridge.handle(.foreground, host: host).wait == nil)
        #expect(host.take() == [.setBackground(false)])
        #expect(bridge.finished(wait!, result: 1, host: host).wait == nil)
        #expect(host.take() == [.post(.stop), .post(.trimMemory(level: 20)), .post(.saveInstanceState)])
    }

    /// An app that ends with the focus keeps its window and open gate through onPause, as on Android; the gate closes
    /// after the focus's post, and the window goes before onNativeWindowDestroyed (the c6 host fixes).
    @Test(arguments: [Lifecycle.Event.quit, .finish])
    func anEndKeepsTheWindowUntilTheFocusIsGoneAndWaitsForOnDestroy(end: Lifecycle.Event) {
        let host = FakeHost()
        var bridge = Self.running(host)

        let first = bridge.handle(end, host: host).wait
        #expect(host.take() == [
            .post(.pause), .post(.windowFocusChanged(false)), .setGate(1, open: false), .removeWindow(1),
            .post(.nativeWindowDestroyed(window: 1)),
        ])
        #expect(first?.callback == .onNativeWindowDestroyed)

        let second = bridge.finished(first!, result: 1, host: host).wait
        #expect(host.take() == [.post(.stop), .post(.trimMemory(level: 20)), .post(.destroy)])
        #expect(second?.callback == .onDestroy)
        #expect(!bridge.lifecycle.hasEnded)

        #expect(bridge.finished(second!, result: 0, host: host).wait == nil)
        #expect(host.take() == [])
        #expect(bridge.lifecycle.hasEnded)
    }

    @Test func quitInTheBackgroundOnlyDestroys() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        let wait = bridge.handle(.background, host: host).wait
        _ = bridge.finished(wait!, result: 1, host: host).wait
        _ = host.take()

        let destroy = bridge.handle(.finish, host: host).wait
        #expect(host.take() == [.post(.destroy)])
        #expect(destroy?.callback == .onDestroy)
    }

    /// A timed-out wait's answer that comes late must not answer a later wait for the same callback.
    @Test func aLateAnswerToAnEarlierWaitChangesNothing() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        let early = bridge.handle(.background, host: host).wait!
        _ = bridge.finished(early, result: 0, host: host).wait
        bridge.handle(.foreground, host: host)
        bridge.handle(.active, host: host)
        bridge.handle(.menuClosed, host: host)
        bridge.handle(.inactive, host: host)
        let later = bridge.handle(.background, host: host).wait!
        _ = host.take()
        #expect(early.callback == later.callback)
        #expect(early.serial != later.serial)

        #expect(bridge.finished(early, result: 1, host: host).wait == nil)
        #expect(host.take() == [])
        #expect(bridge.lifecycle.awaits == .onNativeWindowDestroyed)
        _ = bridge.finished(later, result: 1, host: host).wait
        #expect(host.take() == [.post(.stop), .post(.trimMemory(level: 20)), .post(.saveInstanceState)])
    }

    @Test func aWaitThatTimesOutOrBreaksGoesOn() {
        for result: Int32 in [0, -1] {
            let host = FakeHost()
            var bridge = Self.running(host)
            bridge.handle(.inactive, host: host)
            let wait = bridge.handle(.background, host: host).wait!
            _ = host.take()

            _ = bridge.finished(wait, result: result, host: host).wait
            #expect(host.take() == [.post(.stop), .post(.trimMemory(level: 20)), .post(.saveInstanceState)])
        }
    }

    @Test func aBrokenChannelStopsEveryCall() {
        let host = FakeHost()
        host.breaksAt = 2
        var bridge = Self.bridge()

        #expect(bridge.handle(.start, host: host).wait == nil)
        #expect(host.take() == [.addWindow(width: Self.width, height: Self.height), .post(.create), .post(.start)])
        #expect(bridge.isBroken)

        bridge.handle(.active, host: host)
        bridge.handle(.quit, host: host)
        #expect(host.take() == [])
    }

    @Test func memoryWarningPostsLowMemoryOnceStarted() {
        let host = FakeHost()
        var bridge = Self.bridge()
        bridge.handle(.memoryWarning, host: host)
        #expect(host.take() == [])

        bridge = Self.running(host)
        bridge.handle(.memoryWarning, host: host)
        #expect(host.take() == [.post(.lowMemory)])
    }

    @Test func memoryWarningsAreRateLimitedPerGcMinInterval() {
        let host = FakeHost()
        var bridge = Self.running(host)
        // A device run forwarded ten warnings in eleven seconds (one per iOS kernel event). AOSP's AppProfiler
        // passes low memory on at most once per GC_MIN_INTERVAL (60 s), so the guest must get ONE onLowMemory.
        bridge.handle(.memoryWarning, host: host, now: .seconds(0))
        for _ in 0 ..< 9 { bridge.handle(.memoryWarning, host: host, now: .seconds(4)) }
        #expect(host.take() == [.post(.lowMemory)])
        // Another inside 60 s is dropped; one at/after 60 s passes again.
        bridge.handle(.memoryWarning, host: host, now: .seconds(59))
        #expect(host.take() == [])
        bridge.handle(.memoryWarning, host: host, now: .seconds(61))
        #expect(host.take() == [.post(.lowMemory)])
    }

    @Test func backgroundBeforeTheStartOnlyTellsTheRuntime() {
        let host = FakeHost()
        var bridge = Self.bridge()

        bridge.handle(.background, host: host)
        #expect(host.take() == [.setBackground(true), .waitScheduled(SessionBridge.scheduledTimeout)])
        bridge.handle(.start, host: host)
        #expect(host.take() == [])
        bridge.handle(.foreground, host: host)
        #expect(host.take() == [
            .setBackground(false), .addWindow(width: Self.width, height: Self.height),
            .post(.create), .post(.start), .addInputChannel, .post(.resume), .post(.inputQueueCreated(channel: 7)),
        ] + Self.shown(1, first: true))
    }

    /// The background steps run once per trip, also when iOS reports it twice.
    @Test func aSecondBackgroundChangesNothing() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        let wait = bridge.handle(.background, host: host).wait!
        _ = bridge.finished(wait, result: 1, host: host).wait
        _ = host.take()

        bridge.handle(.background, host: host)
        #expect(host.take() == [])
    }

    /// The callbacks the guest was sent, for the log: those whose post went through.
    @Test func handlingReturnsTheCallbacksPosted() {
        let host = FakeHost()
        var bridge = Self.bridge()
        #expect(bridge.handle(.start, host: host).callbacks == [.onCreate, .onStart, .onResume, .onNativeWindowCreated])

        let breaking = FakeHost()
        breaking.breaksAt = 2
        bridge = Self.bridge()
        #expect(bridge.handle(.start, host: breaking).callbacks == [.onCreate])
    }

    @Test func requestsBecomeLifecycleEventsOrSettings() {
        #expect(SessionRequest.finish.event == .finish)
        #expect(SessionRequest.keepScreenOn(true).event == nil)
        #expect(SessionRequest.startFailed("dlopen failed").event == nil)
        #expect(SessionRequest.frameRate(60).event == nil)
        #expect(SessionRequest.windowOpaque(false).event == nil)
        #expect(SessionRequest.windowOpaque(true).event == nil)
    }

    /// APKRT_REQUEST_FRAME_RATE's argument holds a float's bits (ADR-0036, decision 2).
    @Test func theFrameRateComesAsAFloatsBits() {
        #expect(SessionRequest.frameRate(bits: UInt64(Float(90).bitPattern)) == .frameRate(90))
        #expect(SessionRequest.frameRate(bits: 0) == .frameRate(0))
        // APKRT_REQUEST_STARTING_WINDOW: a0 in the low 32 bits, a1 in the high ones (ADR-0076).
        #expect(SessionRequest.startingWindow(packed: 0xFFFF_5722_FF2E_7D32)
            == .startingWindow(splash: 0xFF2E_7D32, iconBackground: 0xFFFF_5722))
        #expect(SessionRequest.startingWindow(packed: 0xFF15_65C0) == .startingWindow(splash: 0xFF15_65C0, iconBackground: 0))
    }

    // ADR-0038, decision 1.4 and test 2: a layout in another size turns the running window, in rotation-test's
    // order on Android 15: CONFIG_CHANGED, then the same window resized, its content's rectangle, redraw needed.

    /// The window takes its new size between the configuration and the resize, as ADR-0038 c) has
    /// `apkrt_window_set_size`: from the resize on, the guest's ANativeWindow answers the new size.
    @Test func aTurnPostsTheConfigurationAndTheNewSizeToTheSameWindow() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.turns = true

        bridge.windowLaidOut(PixelSize(width: Self.height, height: Self.width), host: host)
        #expect(host.take() == [
            .post(.configurationChanged),
            .setWindowSize(1, width: Self.height, height: Self.width),
            .post(.nativeWindowResized(window: 1, width: Self.height, height: Self.width)),
            .post(.contentRectChanged(left: 0, top: 0, right: Self.height, bottom: Self.width)),
            .post(.nativeWindowRedrawNeeded(window: 1)),
        ])
        #expect(bridge.window == 1)
        #expect(bridge.windowSize == PixelSize(width: Self.height, height: Self.width))
    }

    /// The bars floating windows are laid out inside go to the guest before the configuration (ADR-0041 decision 8), so
    /// a dialog laid out again at the turn centers between the new ones: APKRT_EVENT_BAR_INSETS.
    @Test func aTurnPostsTheNewBarsBeforeTheConfiguration() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.turns = true
        bridge.systemBars = GuestInsets.Edges(left: 0, top: 72, right: 0, bottom: 63)

        bridge.windowLaidOut(PixelSize(width: Self.height, height: Self.width), host: host)
        #expect(host.take() == [
            .post(.barInsets(top: 72, bottom: 63)),
            .post(.configurationChanged),
            .setWindowSize(1, width: Self.height, height: Self.width),
            .post(.nativeWindowResized(window: 1, width: Self.height, height: Self.width)),
            .post(.contentRectChanged(left: 0, top: 0, right: Self.height, bottom: Self.width)),
            .post(.nativeWindowRedrawNeeded(window: 1)),
        ])
    }

    /// Size compat keeps the guest's window, and its bars with it.
    @Test func aLayoutTheGuestDoesNotTakePostsNoBars() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.systemBars = GuestInsets.Edges(left: 0, top: 72, right: 0, bottom: 63)

        bridge.windowLaidOut(PixelSize(width: Self.height, height: Self.width), host: host)
        #expect(host.take() == [])
    }

    /// A turn between the landscape sides keeps the window's size: nothing changes for the guest, as on Android.
    @Test func aLayoutOfTheSameSizeGivesNothing() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.turns = true

        bridge.windowLaidOut(PixelSize(width: Self.width, height: Self.height), host: host)
        #expect(host.take() == [])
    }

    /// Before the start and without a window, a layout only sets the size the next window gets.
    @Test func aLayoutWithoutAWindowSetsTheNextWindowsSize() {
        let host = FakeHost()
        var bridge = Self.bridge()
        bridge.turns = true
        bridge.windowLaidOut(PixelSize(width: Self.height, height: Self.width), host: host)
        #expect(host.take() == [])

        bridge.handle(.start, host: host)
        #expect(host.take().first == .addWindow(width: Self.height, height: Self.width))
    }

    /// A session that holds its orientation does not turn its window (ADR-0038, decision 1.3): a layout in another
    /// size, as iOS gives one late, keeps the guest's size in size compat (ADR-0041, decision 12 b).
    @Test func aSessionThatHoldsItsOrientationKeepsTheGuestsSize() {
        let host = FakeHost()
        var bridge = Self.running(host)

        #expect(bridge.windowLaidOut(PixelSize(width: Self.height, height: Self.width), host: host) == .init())
        #expect(host.take() == [])
        #expect(bridge.windowSize == PixelSize(width: Self.height, height: Self.width))
        #expect(bridge.guestSize == Self.fullSize)
    }

    /// setRequestedOrientation in a session that holds its orientation (APKRT_REQUEST_ORIENTATION): the layout in the
    /// orientation the app asked for turns its window as in a session that turns, so Java hears the configuration and
    /// the resize; and the layout back at the next request turns it back.
    @Test func aRequestedOrientationTurnsASessionThatHoldsItsOwn() {
        let host = FakeHost()
        var bridge = Self.running(host)

        bridge.requested = .landscape
        bridge.windowLaidOut(PixelSize(width: Self.height, height: Self.width), host: host)
        #expect(host.take() == [
            .post(.configurationChanged),
            .setWindowSize(1, width: Self.height, height: Self.width),
            .post(.nativeWindowResized(window: 1, width: Self.height, height: Self.width)),
            .post(.contentRectChanged(left: 0, top: 0, right: Self.height, bottom: Self.width)),
            .post(.nativeWindowRedrawNeeded(window: 1)),
        ])
        #expect(bridge.guestSize == PixelSize(width: Self.height, height: Self.width))

        bridge.requested = .portrait
        bridge.windowLaidOut(Self.fullSize, host: host)
        #expect(host.take() == [
            .post(.configurationChanged),
            .setWindowSize(1, width: Self.width, height: Self.height),
            .post(.nativeWindowResized(window: 1, width: Self.width, height: Self.height)),
            .post(.contentRectChanged(left: 0, top: 0, right: Self.width, bottom: Self.height)),
            .post(.nativeWindowRedrawNeeded(window: 1)),
        ])
    }

    /// A layout against the orientation the app asked for, as iOS gives one late, keeps the guest's size as before.
    @Test func aLayoutAgainstTheRequestedOrientationKeepsTheGuestsSize() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.requested = .portrait

        bridge.windowLaidOut(PixelSize(width: Self.height, height: Self.width), host: host)
        #expect(host.take() == [])
        #expect(bridge.guestSize == Self.fullSize)
    }

    /// APKRT_REQUEST_ORIENTATION's argument, the value sign-extended: -1 is SCREEN_ORIENTATION_UNSPECIFIED.
    @Test func anOrientationRequestCarriesTheSignedValue() {
        #expect(SessionRequest.orientation(argument: UInt64.max) == .orientation(-1))
        #expect(SessionRequest.orientation(argument: 6) == .orientation(6))
        #expect(SessionRequest.orientation(6).event == nil)
    }

    // ADR-0041, decision 12: a size change outside a turn, as the iPhone Duo's fold makes one.

    static let duoOpen = PixelSize(width: 2007, height: 2853)
    static let duoFolded = PixelSize(width: 1398, height: 2034)

    /// a) An activity that handles the change gets a turn's order with the new size.
    @Test func aSizeChangeTheActivityHandlesReachesItAsATurnDoes() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.resizes = true

        bridge.windowLaidOut(Self.duoOpen, host: host)
        #expect(host.take() == [
            .post(.configurationChanged),
            .setWindowSize(1, width: 2007, height: 2853),
            .post(.nativeWindowResized(window: 1, width: 2007, height: 2853)),
            .post(.contentRectChanged(left: 0, top: 0, right: 2007, bottom: 2853)),
            .post(.nativeWindowRedrawNeeded(window: 1)),
        ])
        #expect(bridge.guestSize == Self.duoOpen)
        #expect(bridge.letterbox == nil)
    }

    /// b) Without, the guest keeps its window and size, and the host shows it as Android 15's size compat
    /// (ActivityRecord): not scaled when it fits, centered across, at the top (the letterbox's position
    /// multipliers 0.5 and 0).
    @Test func withoutTheChangesTheGuestKeepsItsSizeInSizeCompat() {
        let host = FakeHost()
        var bridge = Self.running(host)

        #expect(bridge.windowLaidOut(Self.duoOpen, host: host) == .init())
        #expect(host.take() == [])
        #expect(bridge.guestSize == Self.fullSize)
        #expect(bridge.letterbox == Letterbox(frame: PixelRect(left: 359, top: 0, right: 1649, bottom: 2796), scale: 1,
                                             size: Self.fullSize))
    }

    /// A guest larger than the window is scaled down to fit, never up.
    @Test func aGuestLargerThanTheWindowIsScaledDown() {
        let host = FakeHost()
        var bridge = Self.running(host)

        bridge.windowLaidOut(Self.duoFolded, host: host)
        #expect(host.take() == [])
        #expect(bridge.letterbox == Letterbox(frame: PixelRect(left: 230, top: 0, right: 1168, bottom: 2034),
                                             scale: Float(2034) / Float(2796), size: Self.fullSize))
    }

    /// Back at the guest's size, size compat ends, and the guest hears nothing.
    @Test func backAtTheGuestsSizeSizeCompatEnds() {
        let host = FakeHost()
        var bridge = Self.running(host)

        bridge.windowLaidOut(Self.duoOpen, host: host)
        bridge.windowLaidOut(Self.fullSize, host: host)
        #expect(host.take() == [])
        #expect(bridge.letterbox == nil)
    }

    /// The window of the foreground has the guest's size while it is in size compat, as Android keeps an
    /// activity's size until it is created anew.
    @Test func aNewWindowInSizeCompatHasTheGuestsSize() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.windowLaidOut(Self.duoOpen, host: host)
        bridge.handle(.inactive, host: host)
        let wait = bridge.handle(.background, host: host).wait
        bridge.finished(try #require(wait), result: 1, host: host)
        _ = host.take()

        bridge.handle(.foreground, host: host)
        bridge.handle(.menuClosed, host: host)
        #expect(host.take().contains(.addWindow(width: Self.width, height: Self.height)))
        #expect(bridge.letterbox == Letterbox(frame: PixelRect(left: 359, top: 0, right: 1649, bottom: 2796), scale: 1,
                                             size: Self.fullSize))
    }

    /// A session that turns but does not handle the other changes keeps its size at a fold: a turn changes the
    /// orientation and the screen size only, a fold also the smallest width and the screen layout.
    @Test func aTurningSessionKeepsItsSizeAtAFold() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.turns = true

        bridge.windowLaidOut(Self.duoOpen, host: host)
        #expect(host.take() == [])
        #expect(bridge.guestSize == Self.fullSize)
    }

    /// Before the guest has a window, any layout sets its size.
    @Test func beforeTheStartALayoutSetsTheGuestsSize() {
        let host = FakeHost()
        var bridge = Self.bridge()
        bridge.windowLaidOut(Self.duoOpen, host: host)
        #expect(bridge.guestSize == Self.duoOpen)
        #expect(bridge.letterbox == nil)

        bridge.handle(.start, host: host)
        #expect(host.take().first == .addWindow(width: 2007, height: 2853))
    }

    /// ActivityInfo's CONFIG_SCREEN_LAYOUT, CONFIG_SCREEN_SIZE and CONFIG_SMALLEST_SCREEN_SIZE, all three; below
    /// targetSdk 13 Android adds the two sizes (ActivityInfo.getRealConfigChanged), so screenLayout alone is enough
    /// there.
    @Test func anActivityResizesWhenItsConfigChangesHoldTheSizes() {
        #expect(SessionBridge.resizes(configChanges: 0x0D00, targetSdk: 35))
        #expect(SessionBridge.resizes(configChanges: 0x0DB0, targetSdk: 35))
        #expect(!SessionBridge.resizes(configChanges: 0x0C00, targetSdk: 35))
        #expect(!SessionBridge.resizes(configChanges: 0x0500, targetSdk: 35))
        #expect(!SessionBridge.resizes(configChanges: nil, targetSdk: 35))
        #expect(SessionBridge.resizes(configChanges: 0x0100, targetSdk: 12))
        #expect(!SessionBridge.resizes(configChanges: 0x0100, targetSdk: 13))
    }

    /// A turn while the window is gone: the window of the foreground has the new size.
    @Test func aTurnInTheBackgroundGivesTheNextWindowTheNewSize() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.turns = true
        bridge.handle(.inactive, host: host)
        let wait = bridge.handle(.background, host: host).wait
        _ = host.take()

        bridge.windowLaidOut(PixelSize(width: Self.height, height: Self.width), host: host)
        #expect(host.take() == [])
        bridge.finished(try #require(wait), result: 1, host: host)
        bridge.handle(.foreground, host: host)
        bridge.handle(.menuClosed, host: host)
        #expect(host.take().contains(.addWindow(width: Self.height, height: Self.width)))
    }

    // MARK: ADR-0043 c4 (d): a SurfaceView's surfaces, answered as apk-run's headless host answers them

    static let full = PixelRect(left: 0, top: 0, right: width, bottom: height)
    static let fullSize = PixelSize(width: width, height: height)

    static func make(_ token: UInt64, z: Int32 = -2, opaque: Bool = true) -> SurfaceRequest {
        SurfaceRequest(operation: .make, token: token, z: z, frame: full, size: fullSize, opaque: opaque)
    }

    static func place(_ token: UInt64, z: Int32 = -2, frame: PixelRect = full, size: PixelSize = fullSize)
        -> SurfaceRequest {
        SurfaceRequest(operation: .place, token: token, z: z, frame: frame, size: size, opaque: true)
    }

    /// A remove carries only the op and the token (apkrt.h).
    static func remove(_ token: UInt64) -> SurfaceRequest {
        SurfaceRequest(operation: .remove, token: token, z: 0, frame: PixelRect(left: 0, top: 0, right: 0, bottom: 0),
                       size: PixelSize(width: 0, height: 0), opaque: false)
    }

    /// A make gets a window on a layer of its own, then SURFACE_CREATED with the window and the buffer's size; its
    /// gate opens, as the window's is open.
    @Test func aMakeGetsAWindowThenCreatedAndTheWindowsGate() {
        let host = FakeHost()
        var bridge = Self.running(host)

        bridge.surfaceRequested(Self.make(5), host: host)
        #expect(host.take() == [
            .addSurface(frame: Self.full, z: -2, size: Self.fullSize, opaque: true),
            .post(.surfaceCreated(token: 5, window: 2, width: Self.width, height: Self.height)),
            .setGate(2, open: true),
        ])
        #expect(bridge.surfaceWindows == [2])
    }

    /// A surface's gate follows the window's: closed at inactive and with the menu, open again with the focus.
    @Test func aSurfacesGateFollowsTheWindows() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        _ = host.take()

        bridge.surfaceRequested(Self.make(5, z: 1, opaque: false), host: host)
        #expect(host.take() == [
            .addSurface(frame: Self.full, z: 1, size: Self.fullSize, opaque: false),
            .post(.surfaceCreated(token: 5, window: 2, width: Self.width, height: Self.height)),
        ])
        bridge.handle(.active, host: host)
        #expect(host.take() == [.post(.windowFocusChanged(true)), .setGate(1, open: true), .setGate(2, open: true)])
        bridge.handle(.menuOpened, host: host)
        #expect(host.take() == [
            .post(.pause), .post(.windowFocusChanged(false)), .setGate(1, open: false), .setGate(2, open: false),
        ])
    }

    /// A place moves the surface's layer; another size also gives its window that size, then SURFACE_RESIZED; a move
    /// alone gets no event, as apk-run's.
    @Test func aPlaceMovesTheLayerAndAnotherSizeResizesTheWindow() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.surfaceRequested(Self.make(5), host: host)
        _ = host.take()

        let lower = PixelRect(left: 0, top: 100, right: Self.width, bottom: Self.height + 100)
        bridge.surfaceRequested(Self.place(5, frame: lower), host: host)
        #expect(host.take() == [.placeSurface(2, frame: lower, z: -2)])

        let half = PixelRect(left: 0, top: 0, right: Self.width, bottom: Self.height / 2)
        let halfSize = PixelSize(width: Self.width, height: Self.height / 2)
        bridge.surfaceRequested(Self.place(5, z: -1, frame: half, size: halfSize), host: host)
        #expect(host.take() == [
            .placeSurface(2, frame: half, z: -1), .setWindowSize(2, width: Self.width, height: Self.height / 2),
            .post(.surfaceResized(token: 5, window: 2, width: Self.width, height: Self.height / 2)),
        ])
    }

    /// A remove takes the window away without an event; the token is forgotten, so a later place or remove of it
    /// does nothing (the runtime refuses them first).
    @Test func aRemoveTakesTheWindowAwayWithoutAnEvent() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.surfaceRequested(Self.make(5), host: host)
        _ = host.take()

        bridge.surfaceRequested(Self.remove(5), host: host)
        #expect(host.take() == [.removeSurface(2)])
        #expect(bridge.surfaceWindows == [])
        bridge.surfaceRequested(Self.place(5), host: host)
        bridge.surfaceRequested(Self.remove(5), host: host)
        #expect(host.take() == [])
    }

    /// At the background every surface's window goes right after the window's, before PAUSE, and SURFACE_DESTROYED
    /// comes for each in make order after PAUSE and the focus, before NATIVE_WINDOW_DESTROYED and STOP (apkrt.h's
    /// surface handler; apk-run's hide).
    @Test func theBackgroundTakesTheSurfacesWithTheWindow() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.surfaceRequested(Self.make(5), host: host)
        bridge.surfaceRequested(Self.make(6, z: 1), host: host)
        _ = host.take()

        bridge.handle(.inactive, host: host)
        #expect(host.take() == [
            .post(.windowFocusChanged(false)), .setGate(1, open: false), .setGate(2, open: false),
            .setGate(3, open: false),
        ])
        let wait = try #require(bridge.handle(.background, host: host).wait)
        #expect(host.take() == [
            .removeWindow(1), .removeSurface(2), .removeSurface(3), .post(.pause),
            .post(.surfaceDestroyed(token: 5, window: 2)), .post(.surfaceDestroyed(token: 6, window: 3)),
            .post(.nativeWindowDestroyed(window: 1)), .waitAnswer(wait.serial), .setBackground(true),
            .waitScheduled(SessionBridge.scheduledTimeout),
        ])
        #expect(wait.callback == .onNativeWindowDestroyed)
        #expect(bridge.surfaceWindows == [])

        // The guest's place and remove of a surface the host took away still come; the host ignores them.
        bridge.surfaceRequested(Self.place(5), host: host)
        bridge.surfaceRequested(Self.remove(5), host: host)
        bridge.surfaceRequested(Self.remove(6), host: host)
        #expect(host.take() == [])
        bridge.finished(wait, result: 1, host: host)
        #expect(host.take() == [.post(.stop), .post(.trimMemory(level: 20)), .post(.saveInstanceState)])
    }

    /// At the background a surface's window goes until the guest removes the surface, after its surfaceDestroyed
    /// callbacks: its swaps succeed without a frame until then, as a SurfaceView's BufferQueue takes frames until
    /// surfaceDestroyed has returned (SDL backs its context up there, with no failed swap); the next window tells the
    /// surfaces still untold.
    @Test func theBackgroundRemovesASurfaceUntilTheGuestRemovesIt() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.surfaceRequested(Self.make(5), host: host)
        bridge.surfaceRequested(Self.make(6, z: 1), host: host)
        bridge.handle(.inactive, host: host)
        host.answers = [1]
        bridge.handle(.background, host: host)
        #expect(host.surfacesUntilTold == [2, 3])
        #expect(host.told == [1])
        #expect(bridge.untoldSurfaces == [2, 3])
        bridge.surfaceRequested(Self.remove(5), host: host)
        #expect(host.told == [1, 2])
        #expect(bridge.untoldSurfaces == [3])
        bridge.handle(.foreground, host: host)
        bridge.handle(.active, host: host)
        bridge.handle(.menuClosed, host: host)
        #expect(host.told == [1, 2, 3])
        #expect(bridge.untoldSurfaces.isEmpty)
    }

    /// An app that ends with the focus: its surfaces go with the window, after PAUSE and the focus, and their
    /// SURFACE_DESTROYED comes before NATIVE_WINDOW_DESTROYED.
    @Test(arguments: [Lifecycle.Event.quit, .finish])
    func anEndTakesTheSurfacesWithTheWindow(end: Lifecycle.Event) {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.surfaceRequested(Self.make(5), host: host)
        _ = host.take()

        bridge.handle(end, host: host)
        #expect(host.take() == [
            .post(.pause), .post(.windowFocusChanged(false)), .setGate(1, open: false), .setGate(2, open: false),
            .removeWindow(1), .removeSurface(2), .post(.surfaceDestroyed(token: 5, window: 2)),
            .post(.nativeWindowDestroyed(window: 1)),
        ])
    }

    /// ADR-0062: an open request's answer goes to the guest as long as the session runs; the bridge says whether the
    /// runtime was told of the background, when nothing may be shown.
    @Test func anOpenAnswerIsPostedWhileTheSessionRuns() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        _ = host.take()
        #expect(!bridge.isInBackground)
        bridge.openAnswered(id: 7, .shown, host: host)
        #expect(host.take() == [.post(.openResult(id: 7, outcome: .shown))])
        bridge.handle(.inactive, host: host)
        bridge.finished(try #require(bridge.handle(.background, host: host).wait), result: 1, host: host)
        #expect(bridge.isInBackground)
        _ = host.take()
        bridge.openAnswered(id: 8, .background, host: host)
        #expect(host.take() == [.post(.openResult(id: 8, outcome: .background))])
    }

    /// ADR-0057 step b: a pick's answer goes to the guest as long as the session runs, and not after its end.
    @Test func aPickAnswerIsPostedWhileTheSessionRuns() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        _ = host.take()
        bridge.pickAnswered(id: 3, picked: 2, host: host)
        #expect(host.take() == [.post(.pickResult(id: 3, code: -1, count: 2))])
        bridge.pickAnswered(id: 4, picked: nil, host: host)
        #expect(host.take() == [.post(.pickResult(id: 4, code: 0, count: 0))])
        let first = try #require(bridge.handle(.quit, host: host).wait)
        let second = try #require(bridge.finished(first, result: 1, host: host).wait)
        _ = bridge.finished(second, result: 0, host: host)
        #expect(bridge.lifecycle.hasEnded)
        _ = host.take()
        bridge.pickAnswered(id: 5, picked: 1, host: host)
        #expect(host.take() == [])
    }

    /// An install's answer carries a failure's reason (APKRT_EVENT_INSTALL_RESULT's a2), and none for any other
    /// outcome, so that the framework answers the installer with Android's code for it.
    @Test func anInstallAnswerCarriesAFailuresReason() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        _ = host.take()
        bridge.installAnswered(id: 1, .failed, reason: .conflict, host: host)
        bridge.installAnswered(id: 2, .failed, host: host)
        bridge.installAnswered(id: 3, .ready, reason: .storage, host: host)
        bridge.installAnswered(id: 4, .cancelled, host: host)
        #expect(host.take() == [.post(.installResult(id: 1, outcome: .failed, reason: .conflict)),
                                .post(.installResult(id: 2, outcome: .failed, reason: .none)),
                                .post(.installResult(id: 3, outcome: .ready, reason: .none)),
                                .post(.installResult(id: 4, outcome: .cancelled, reason: .none))])
    }

    /// ADR-0077: an uninstall request's answer goes to the guest once, as long as the session runs, and not after its
    /// end (an asking app waits for it, as for Android's uninstaller).
    @Test func anUninstallAnswerIsPostedWhileTheSessionRuns() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        _ = host.take()
        bridge.uninstallAnswered(id: 3, .removed, host: host)
        bridge.uninstallAnswered(id: 4, .failed, host: host)
        bridge.uninstallAnswered(id: 5, .cancelled, host: host)
        #expect(host.take() == [.post(.uninstallResult(id: 3, outcome: .removed)),
                                .post(.uninstallResult(id: 4, outcome: .failed)),
                                .post(.uninstallResult(id: 5, outcome: .cancelled))])
        #expect(UninstallOutcome.removed.rawValue == 0 && UninstallOutcome.failed.rawValue == 1
                && UninstallOutcome.cancelled.rawValue == 2)
        let first = try #require(bridge.handle(.quit, host: host).wait)
        let second = try #require(bridge.finished(first, result: 1, host: host).wait)
        _ = bridge.finished(second, result: 0, host: host)
        _ = host.take()
        bridge.uninstallAnswered(id: 6, .removed, host: host)
        #expect(host.take() == [])
    }

    /// ADR-0068 step 2d: the keyboard's cover goes to the guest when it changes, as long as the session runs; UIKit's
    /// repeats of the same frame post nothing.
    @Test func theKeyboardsCoverIsPostedWhenItChanges() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        _ = host.take()
        bridge.keyboardCovers(pixels: 0, host: host)
        #expect(host.take() == [])
        bridge.keyboardCovers(pixels: 1008, host: host)
        bridge.keyboardCovers(pixels: 1008, host: host)
        #expect(host.take() == [.post(.imeInsets(bottom: 1008))])
        bridge.keyboardCovers(pixels: 0, host: host)
        #expect(host.take() == [.post(.imeInsets(bottom: 0))])
        let first = try #require(bridge.handle(.quit, host: host).wait)
        let second = try #require(bridge.finished(first, result: 1, host: host).wait)
        _ = bridge.finished(second, result: 0, host: host)
        _ = host.take()
        bridge.keyboardCovers(pixels: 900, host: host)
        #expect(host.take() == [])
    }

    /// The system's back (ADR-0041 decision 10), as a button sends it: posted while the app runs, not after its
    /// end.
    @Test func theSystemsBackIsPostedWhileTheAppRuns() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        _ = host.take()
        bridge.back(.commit, progress: 0, edge: 0, host: host)
        #expect(host.take() == [.post(.back(phase: .commit, progress: 0, edge: 0))])
        let first = try #require(bridge.handle(.quit, host: host).wait)
        let second = try #require(bridge.finished(first, result: 1, host: host).wait)
        _ = bridge.finished(second, result: 0, host: host)
        _ = host.take()
        bridge.back(.commit, progress: 0, edge: 0, host: host)
        #expect(host.take() == [])
    }

    /// The user's hide of the keyboard (ADR-0068, APKRT_EVENT_IME_HIDDEN): posted while the app runs, once per hide,
    /// not after its end.
    @Test func theUsersHideOfTheKeyboardIsPostedWhileTheAppRuns() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        _ = host.take()
        bridge.keyboardHidden(host: host)
        #expect(host.take() == [.post(.imeHidden)])
        bridge.keyboardHidden(host: host)
        #expect(host.take() == [.post(.imeHidden)])
        let first = try #require(bridge.handle(.quit, host: host).wait)
        let second = try #require(bridge.finished(first, result: 1, host: host).wait)
        _ = bridge.finished(second, result: 0, host: host)
        _ = host.take()
        bridge.keyboardHidden(host: host)
        #expect(host.take() == [])
    }

    /// A surface made in the background gets its window with a closed gate, which opens after the window's at the
    /// return.
    @Test func aSurfaceMadeInTheBackgroundOpensWithTheReturn() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        bridge.finished(try #require(bridge.handle(.background, host: host).wait), result: 1, host: host)
        _ = host.take()

        bridge.surfaceRequested(Self.make(5), host: host)
        #expect(host.take() == [
            .addSurface(frame: Self.full, z: -2, size: Self.fullSize, opaque: true),
            .post(.surfaceCreated(token: 5, window: 2, width: Self.width, height: Self.height)),
        ])
        bridge.handle(.foreground, host: host)
        bridge.handle(.active, host: host)
        bridge.handle(.menuClosed, host: host)
        #expect(host.take().suffix(3) == [
            .post(.windowFocusChanged(true)), .setGate(3, open: true), .setGate(2, open: true),
        ])
        #expect(bridge.surfaceWindows == [2])
    }

    /// An end in the background takes a surface made there away before onDestroy, without an event (apk-run's
    /// take_away at the end).
    @Test func anEndInTheBackgroundTakesTheSurfacesBeforeOnDestroy() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        bridge.finished(try #require(bridge.handle(.background, host: host).wait), result: 1, host: host)
        bridge.surfaceRequested(Self.make(5), host: host)
        _ = host.take()

        bridge.handle(.quit, host: host)
        #expect(host.take() == [.removeSurface(2), .post(.destroy)])
        #expect(bridge.surfaceWindows == [])
    }

    /// A request after the end is dropped: its answer would never come (the c4 plan's risks).
    @Test func aRequestAfterTheEndIsDropped() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        let first = try #require(bridge.handle(.quit, host: host).wait)
        let second = try #require(bridge.finished(first, result: 1, host: host).wait)
        bridge.finished(second, result: 1, host: host)
        #expect(bridge.lifecycle.hasEnded)
        _ = host.take()

        bridge.surfaceRequested(Self.make(5), host: host)
        #expect(host.take() == [])
    }

    /// A surface the host has no window for gets no answer, and the bridge does not keep it.
    @Test func aSurfaceTheHostRefusesGetsNoEvent() {
        let host = FakeHost()
        var bridge = Self.running(host)
        host.refusesSurfaces = true

        bridge.surfaceRequested(Self.make(5), host: host)
        #expect(host.take() == [.addSurface(frame: Self.full, z: -2, size: Self.fullSize, opaque: true)])
        #expect(bridge.surfaceWindows == [])
        bridge.surfaceRequested(Self.remove(5), host: host)
        #expect(host.take() == [])
    }

    /// SURFACE_CREATED's post that fails breaks the channel, as any post does: the gate stays shut.
    @Test func aFailedCreatedPostBreaksTheChannel() {
        let host = FakeHost()
        var bridge = Self.running(host)
        host.breaksAt = 10

        bridge.surfaceRequested(Self.make(5), host: host)
        #expect(host.take() == [
            .addSurface(frame: Self.full, z: -2, size: Self.fullSize, opaque: true),
            .post(.surfaceCreated(token: 5, window: 2, width: Self.width, height: Self.height)),
        ])
        #expect(bridge.isBroken)
    }

    // MARK: ADR-0041: the guest's own windows (dialogs, popups, toasts), as apk-run's headless host answers them

    static let dialogFrame = PixelRect(left: 100, top: 900, right: 1190, bottom: 1900)
    static let dialogSize = PixelSize(width: 1090, height: 1000)

    static func window(_ operation: WindowRequest.Operation, _ token: UInt64, type: Int32 = 2, parent: UInt64 = 0,
                       frame: PixelRect = dialogFrame, size: PixelSize = dialogSize, alpha: Float = 1,
                       dim: Float = 0.6) -> WindowRequest {
        WindowRequest(operation: operation, token: token, type: type, parent: parent, frame: frame, size: size,
                      opaque: false, group: 1, alpha: alpha, dim: dim)
    }

    /// A make gets a window, WINDOW_CREATED with its size, and its gate open with the window's; a place with the same
    /// size moves it without an event, another size gives the window that size and WINDOW_RESIZED; a remove takes it
    /// away without an event (apkrt.h's window handler).
    @Test func aRequestedWindowIsMadePlacedAndRemoved() {
        let host = FakeHost()
        var bridge = Self.running(host)

        bridge.windowRequested(Self.window(.make, 9), host: host)
        #expect(host.take() == [
            .addRequestedWindow(token: 9),
            .post(.windowCreated(token: 9, window: 2, width: Self.dialogSize.width, height: Self.dialogSize.height)),
            .setGate(2, open: true),
        ])
        bridge.windowRequested(Self.window(.place, 9, alpha: 0.5, dim: 0.3), host: host)
        #expect(host.take() == [.placeRequestedWindow(2, frame: Self.dialogFrame, alpha: 0.5, dim: 0.3)])
        let wider = PixelSize(width: 1190, height: 1000)
        bridge.windowRequested(Self.window(.place, 9, size: wider), host: host)
        #expect(host.take() == [
            .placeRequestedWindow(2, frame: Self.dialogFrame, alpha: 1, dim: 0.6),
            .setWindowSize(2, width: 1190, height: 1000),
            .post(.windowResized(token: 9, window: 2, width: 1190, height: 1000)),
        ])
        bridge.windowRequested(Self.window(.remove, 9), host: host)
        #expect(host.take() == [.removeRequestedWindow(2)])
    }

    /// An activity's own window (type 1, a translucent or floating activity's, ViewRootImpl.requestActivityWindow) is
    /// one that takes the starting window away at its first frame (ADR-0076 decision 1); a dialog's is not.
    @Test func anActivitysOwnRequestedWindowIsAnActivityWindow() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.windowRequested(Self.window(.make, 9), host: host)
        bridge.windowRequested(Self.window(.make, 10, type: 1), host: host)
        bridge.windowRequested(Self.window(.make, 11, type: 1000, parent: 10), host: host)
        _ = host.take()

        #expect(bridge.activityWindows == [3])
        bridge.windowRequested(Self.window(.remove, 11), host: host)
        bridge.windowRequested(Self.window(.remove, 10), host: host)
        #expect(bridge.activityWindows == [])
    }

    /// The requested windows' gates follow the window's, as the surfaces' do.
    @Test func aRequestedWindowsGateFollowsTheWindows() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.windowRequested(Self.window(.make, 9), host: host)
        _ = host.take()

        bridge.handle(.inactive, host: host)
        #expect(host.take() == [.post(.windowFocusChanged(false)), .setGate(1, open: false), .setGate(2, open: false)])
        bridge.handle(.active, host: host)
        #expect(host.take() == [.post(.windowFocusChanged(true)), .setGate(1, open: true), .setGate(2, open: true)])
    }

    /// At the background every requested window goes with the window and the surfaces, before PAUSE; WINDOW_DESTROYED
    /// comes after PAUSE, the focus and the surfaces' SURFACE_DESTROYED, before NATIVE_WINDOW_DESTROYED, a
    /// sub-window's before its parent's (apkrt.h's window handler; apk-run's take_away and post_destroyed). The
    /// guest's later place and remove change nothing.
    @Test func theBackgroundTakesTheRequestedWindowsWithTheWindow() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.surfaceRequested(Self.make(5), host: host)
        bridge.windowRequested(Self.window(.make, 9), host: host)
        bridge.windowRequested(Self.window(.make, 10, type: 1000, parent: 9), host: host)
        bridge.handle(.inactive, host: host)
        _ = host.take()

        let wait = try #require(bridge.handle(.background, host: host).wait)
        #expect(host.take() == [
            .removeWindow(1), .removeSurface(2), .removeRequestedWindow(3), .removeRequestedWindow(4), .post(.pause),
            .post(.surfaceDestroyed(token: 5, window: 2)), .post(.windowDestroyed(token: 10, window: 4)),
            .post(.windowDestroyed(token: 9, window: 3)), .post(.nativeWindowDestroyed(window: 1)),
            .waitAnswer(wait.serial), .setBackground(true), .waitScheduled(SessionBridge.scheduledTimeout),
        ])
        bridge.windowRequested(Self.window(.place, 9), host: host)
        bridge.windowRequested(Self.window(.remove, 10), host: host)
        bridge.windowRequested(Self.window(.remove, 9), host: host)
        #expect(host.take() == [])
    }

    /// An app that ends with the focus: its requested windows go with the window, and their WINDOW_DESTROYED comes
    /// before NATIVE_WINDOW_DESTROYED.
    @Test(arguments: [Lifecycle.Event.quit, .finish])
    func anEndTakesTheRequestedWindowsWithTheWindow(end: Lifecycle.Event) {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.windowRequested(Self.window(.make, 9), host: host)
        _ = host.take()

        bridge.handle(end, host: host)
        #expect(host.take() == [
            .post(.pause), .post(.windowFocusChanged(false)), .setGate(1, open: false), .setGate(2, open: false),
            .removeWindow(1), .removeRequestedWindow(2), .post(.windowDestroyed(token: 9, window: 2)),
            .post(.nativeWindowDestroyed(window: 1)),
        ])
    }

    /// A window made in the background opens with the window's gate at the return, after Continue.
    @Test func aWindowMadeInTheBackgroundOpensWithTheReturn() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        bridge.finished(try #require(bridge.handle(.background, host: host).wait), result: 1, host: host)
        bridge.windowRequested(Self.window(.make, 9), host: host)
        _ = host.take()

        bridge.handle(.foreground, host: host)
        bridge.handle(.active, host: host)
        bridge.handle(.menuClosed, host: host)
        #expect(host.take().suffix(3) == [
            .post(.windowFocusChanged(true)), .setGate(3, open: true), .setGate(2, open: true),
        ])
    }

    /// A window made in the background gets its window with a closed gate; at onDestroy it goes without an event, as
    /// the surfaces made there do. After the end every request is dropped.
    @Test func aWindowMadeInTheBackgroundStaysClosedAndGoesAtOnDestroy() throws {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        bridge.finished(try #require(bridge.handle(.background, host: host).wait), result: 1, host: host)
        _ = host.take()

        bridge.windowRequested(Self.window(.make, 9), host: host)
        #expect(host.take() == [
            .addRequestedWindow(token: 9),
            .post(.windowCreated(token: 9, window: 2, width: Self.dialogSize.width, height: Self.dialogSize.height)),
        ])
        let destroy = try #require(bridge.handle(.finish, host: host).wait)
        #expect(host.take() == [.removeRequestedWindow(2), .post(.destroy)])
        bridge.finished(destroy, result: 0, host: host)
        _ = host.take()
        bridge.windowRequested(Self.window(.make, 11), host: host)
        #expect(host.take() == [])
    }
}
