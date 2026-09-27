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
}
