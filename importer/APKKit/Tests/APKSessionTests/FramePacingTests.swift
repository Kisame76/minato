import Testing

@testable import APKSession

/// ADR-0036, decision 2: the display link's ticks and its frame rate range.
@Suite("FramePacing")
struct FramePacingTests {
    @Test func aTickIsInNanosecondsWithThePeriodToTheNextFrame() {
        let tick = FrameTick(timestamp: 100.5, targetTimestamp: 100.5 + 1.0 / 120)

        #expect(tick.time == 100_500_000_000)
        #expect(tick.targetTime == 100_508_333_333)
        #expect(tick.period == 8_333_333)
    }

    @Test func withoutAPreferenceTheRangeIs60To120Preferring120() {
        #expect(FramePacing.range(preferred: 0) == FrameRateRange(minimum: 60, maximum: 120, preferred: 120))
    }

    /// ANativeWindow_setFrameRate: the benchmark pins 60 Hz (ADR-0036, decision 2).
    @Test func aPreferredRateInTheRangeIsKept() {
        #expect(FramePacing.range(preferred: 60) == FrameRateRange(minimum: 60, maximum: 120, preferred: 60))
        #expect(FramePacing.range(preferred: 90) == FrameRateRange(minimum: 60, maximum: 120, preferred: 90))
    }

    @Test func aPreferredRateOutsideTheRangeIsClampedToIt() {
        #expect(FramePacing.range(preferred: 30).preferred == 60)
        #expect(FramePacing.range(preferred: 144).preferred == 120)
        #expect(FramePacing.range(preferred: -1).preferred == 120)
        #expect(FramePacing.range(preferred: .nan).preferred == 120)
        #expect(FramePacing.range(preferred: .infinity).preferred == 120)
    }

    /// ADR-0066 decision 3: with an FPS unlock the link prefers at least the unlock, whatever lower rate the guest
    /// asks.
    @Test(arguments: [Float(0), 30, 60, 90, 120])
    func anUnlockOf120PrefersIt(guest: Float) {
        #expect(FramePacing.range(preferred: guest, unlock: 120)
                == FrameRateRange(minimum: 60, maximum: 120, preferred: 120))
    }

    @Test func anUnlockOf60KeepsAHigherRate() {
        #expect(FramePacing.range(preferred: 30, unlock: 60).preferred == 60)
        #expect(FramePacing.range(preferred: 90, unlock: 60).preferred == 90)
        #expect(FramePacing.range(preferred: 0, unlock: 60).preferred == 120)
    }

    @Test func noUnlockChangesNothing() {
        for rate: Float in [0, 30, 60, 90, 144] {
            #expect(FramePacing.range(preferred: rate, unlock: 0) == FramePacing.range(preferred: rate))
        }
    }

    // The global frame rate limit (Settings > Apps > Frame Rate Limit, 120, 60 or 30 fps; 0 for the screen's highest).

    @Test func aLimitOf60HoldsTheLinkAt60() {
        #expect(FramePacing.range(preferred: 0, limit: 60) == FrameRateRange(minimum: 60, maximum: 60, preferred: 60))
        #expect(FramePacing.range(preferred: 120, limit: 60) == FrameRateRange(minimum: 60, maximum: 60, preferred: 60))
        #expect(FramePacing.range(preferred: 30, limit: 60) == FrameRateRange(minimum: 60, maximum: 60, preferred: 60))
    }

    @Test func aLimitOf30LowersTheMinimumToo() {
        #expect(FramePacing.range(preferred: 0, limit: 30) == FrameRateRange(minimum: 30, maximum: 30, preferred: 30))
        #expect(FramePacing.range(preferred: 60, limit: 30) == FrameRateRange(minimum: 30, maximum: 30, preferred: 30))
    }

    @Test func aLimitOf120OrNoneChangesNothing() {
        for rate: Float in [0, 30, 60, 90, 144] {
            #expect(FramePacing.range(preferred: rate, limit: 120) == FramePacing.range(preferred: rate))
            #expect(FramePacing.range(preferred: rate, limit: 0) == FramePacing.range(preferred: rate))
        }
    }

    /// An app's own FPS unlock (ADR-0066) is the user's choice for that app: it wins over a lower limit, and the app
    /// runs as without one.
    @Test func anUnlockWinsOverALowerLimit() {
        #expect(FramePacing.range(preferred: 0, unlock: 120, limit: 30)
                == FrameRateRange(minimum: 60, maximum: 120, preferred: 120))
        #expect(FramePacing.range(preferred: 30, unlock: 60, limit: 30)
                == FrameRateRange(minimum: 60, maximum: 60, preferred: 60))
        #expect(FramePacing.range(preferred: 0, unlock: 60, limit: 120)
                == FrameRateRange(minimum: 60, maximum: 120, preferred: 120))
    }

    /// The guest's display rate (Display.getRefreshRate): the screen's highest, at most the limit or the unlock.
    @Test(arguments: [
        (Float(120), Float(0), Float(0), Float(120)), (120, 120, 0, 120), (120, 60, 0, 60), (120, 30, 0, 30),
        (60, 0, 0, 60), (60, 120, 0, 60), (60, 30, 0, 30), (120, 30, 120, 120), (120, 30, 60, 60), (60, 30, 120, 60),
    ])
    func theGuestSeesTheLimitedRate(screen: Float, limit: Float, unlock: Float, rate: Float) {
        #expect(FramePacing.displayRate(screenMaximum: screen, limit: limit, unlock: unlock) == rate)
    }

    /// Settings offers only the steps the screen shows: 120, 60 and 30 at 120 Hz, 60 and 30 at 60 Hz.
    @Test func theStepsAreThoseTheScreenShows() {
        #expect(FramePacing.limitSteps(screenMaximum: 120) == [120, 60, 30])
        #expect(FramePacing.limitSteps(screenMaximum: 60) == [60, 30])
        #expect(FramePacing.limitSteps(screenMaximum: 0) == [30])
    }

    /// The stored choice, 0 for none: a step at or above the screen's highest is no limit.
    @Test func theStoredChoiceIsALimitBelowTheScreensHighestOnly() {
        #expect(FramePacing.limit(stored: 0, screenMaximum: 120) == 0)
        #expect(FramePacing.limit(stored: 120, screenMaximum: 120) == 0)
        #expect(FramePacing.limit(stored: 60, screenMaximum: 120) == 60)
        #expect(FramePacing.limit(stored: 30, screenMaximum: 120) == 30)
        #expect(FramePacing.limit(stored: 120, screenMaximum: 60) == 0)
        #expect(FramePacing.limit(stored: 60, screenMaximum: 60) == 0)
        #expect(FramePacing.limit(stored: 45, screenMaximum: 120) == 0)
        #expect(FramePacing.limit(stored: -1, screenMaximum: 120) == 0)
    }
}
