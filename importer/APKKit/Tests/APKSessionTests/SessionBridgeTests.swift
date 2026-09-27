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
        func setBackground(_ on: Bool) { calls.append(.setBackground(on)) }

        func waitScheduled(seconds: Double) -> Bool {
            calls.append(.waitScheduled(seconds))
            return true
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

    /// Started, active and focused, with window 1.
    static func running(_ host: FakeHost) -> SessionBridge {
        var bridge = bridge()
        bridge.handle(.start, host: host)
        bridge.handle(.active, host: host)
        _ = host.take()
        return bridge
    }

    @Test func startAddsTheWindowAndOpensTheGateWithTheFocus() {
        let host = FakeHost()
        var bridge = Self.bridge()

        bridge.handle(.start, host: host)
        #expect(host.take() == [
            .addWindow(width: Self.width, height: Self.height),
            .post(.create), .post(.start), .post(.resume),
            .post(.nativeWindowCreated(window: 1, width: Self.width, height: Self.height)),
        ])
        bridge.handle(.active, host: host)
        #expect(host.take() == [.post(.windowFocusChanged(true)), .setGate(1, open: true)])
    }

    @Test func startWhenAlreadyActiveOpensTheGateAfterTheEvents() {
        let host = FakeHost()
        var bridge = Self.bridge()

        bridge.handle(.active, host: host)
        #expect(host.take() == [])
        bridge.handle(.start, host: host)
        #expect(host.take() == [
            .addWindow(width: Self.width, height: Self.height),
            .post(.create), .post(.start), .post(.resume),
            .post(.nativeWindowCreated(window: 1, width: Self.width, height: Self.height)),
            .post(.windowFocusChanged(true)), .setGate(1, open: true),
        ])
    }

    @Test func theGateClosesBeforeTheFocusGoes() {
        let host = FakeHost()
        var bridge = Self.running(host)

        bridge.handle(.inactive, host: host)
        #expect(host.take() == [.setGate(1, open: false), .post(.windowFocusChanged(false))])
        bridge.handle(.menuOpened, host: host)
        #expect(host.take() == [])
        bridge.handle(.active, host: host)
        #expect(host.take() == [])
        bridge.handle(.menuClosed, host: host)
        #expect(host.take() == [.post(.windowFocusChanged(true)), .setGate(1, open: true)])
    }

    @Test func backgroundWaitsForTheGPUAndRemovesTheWindowBeforeOnPause() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        _ = host.take()

        let wait = bridge.handle(.background, host: host).wait
        #expect(host.take() == [
            .setBackground(true), .waitScheduled(SessionBridge.scheduledTimeout), .removeWindow(1),
            .post(.pause), .post(.nativeWindowDestroyed(window: 1)),
        ])
        #expect(wait == SessionBridge.Wait(serial: 8, callback: .onNativeWindowDestroyed))

        #expect(bridge.finished(wait!, result: 1, host: host).wait == nil)
        #expect(host.take() == [.post(.stop), .post(.saveInstanceState)])
    }

    @Test func foregroundAddsANewWindowAndOpensItsGateOnceActive() {
        let host = FakeHost()
        var bridge = Self.running(host)
        bridge.handle(.inactive, host: host)
        let wait = bridge.handle(.background, host: host).wait
        _ = bridge.finished(wait!, result: 1, host: host).wait
        _ = host.take()

        bridge.handle(.foreground, host: host)
        #expect(host.take() == [
            .setBackground(false), .addWindow(width: Self.width, height: Self.height),
            .post(.start), .post(.resume),
            .post(.nativeWindowCreated(window: 2, width: Self.width, height: Self.height)),
        ])
        bridge.handle(.active, host: host)
        #expect(host.take() == [.post(.windowFocusChanged(true)), .setGate(2, open: true)])
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
        #expect(host.take() == [
            .post(.stop), .post(.saveInstanceState), .addWindow(width: Self.width, height: Self.height),
            .post(.start), .post(.resume),
            .post(.nativeWindowCreated(window: 2, width: Self.width, height: Self.height)),
        ])
    }

    @Test func quitRemovesTheWindowBeforeOnPauseAndWaitsForOnDestroy() {
        let host = FakeHost()
        var bridge = Self.running(host)

        let first = bridge.handle(.quit, host: host).wait
        #expect(host.take() == [
            .setGate(1, open: false), .post(.windowFocusChanged(false)), .removeWindow(1),
            .post(.pause), .post(.nativeWindowDestroyed(window: 1)),
        ])
        #expect(first?.callback == .onNativeWindowDestroyed)

        let second = bridge.finished(first!, result: 1, host: host).wait
        #expect(host.take() == [.post(.stop), .post(.destroy)])
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
        bridge.handle(.inactive, host: host)
        let later = bridge.handle(.background, host: host).wait!
        _ = host.take()
        #expect(early.callback == later.callback)
        #expect(early.serial != later.serial)

        #expect(bridge.finished(early, result: 1, host: host).wait == nil)
        #expect(host.take() == [])
        #expect(bridge.lifecycle.awaits == .onNativeWindowDestroyed)
        _ = bridge.finished(later, result: 1, host: host).wait
        #expect(host.take() == [.post(.stop), .post(.saveInstanceState)])
    }

    @Test func aWaitThatTimesOutOrBreaksGoesOn() {
        for result: Int32 in [0, -1] {
            let host = FakeHost()
            var bridge = Self.running(host)
            bridge.handle(.inactive, host: host)
            let wait = bridge.handle(.background, host: host).wait!
            _ = host.take()

            _ = bridge.finished(wait, result: result, host: host).wait
            #expect(host.take() == [.post(.stop), .post(.saveInstanceState)])
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
            .post(.create), .post(.start), .post(.resume),
            .post(.nativeWindowCreated(window: 1, width: Self.width, height: Self.height)),
        ])
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
    }
}
