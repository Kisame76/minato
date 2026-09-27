import Testing

@testable import APKSession

/// ADR-0032, decision 9: the FPS label's number, from the window's presents counted at the gate
/// (`apkrt_window_presents`), sampled about once a second.
@Suite("FrameRateMeter")
struct FrameRateMeterTests {
    @Test func theFirstSampleHasNothingToCompareWith() {
        var meter = FrameRateMeter()

        #expect(meter.sample(window: 1, presents: 500, at: 10) == nil)
    }

    @Test func theDifferenceOverOneSecondIsTheFrameRate() {
        var meter = FrameRateMeter()
        _ = meter.sample(window: 1, presents: 500, at: 10)

        #expect(meter.sample(window: 1, presents: 620, at: 11) == 120)
        #expect(meter.sample(window: 1, presents: 680, at: 12) == 60)
    }

    /// "About once a second": the difference is divided by the time that really passed.
    @Test func aLateSampleCountsPerSecond() {
        var meter = FrameRateMeter()
        _ = meter.sample(window: 1, presents: 0, at: 10)

        #expect(meter.sample(window: 1, presents: 66, at: 11.1) == 60)
        #expect(meter.sample(window: 1, presents: 125, at: 12.08) == 60)
    }

    /// The gate is closed (the menu, the app switcher): nothing is presented.
    @Test func noPresentsIsZero() {
        var meter = FrameRateMeter()
        _ = meter.sample(window: 1, presents: 300, at: 10)

        #expect(meter.sample(window: 1, presents: 300, at: 11) == 0)
    }

    /// At background the window goes; the next one counts from its own start.
    @Test func aNewWindowStartsItsOwnCount() {
        var meter = FrameRateMeter()
        _ = meter.sample(window: 1, presents: 900, at: 10)

        #expect(meter.sample(window: 2, presents: 30, at: 11) == nil)
        #expect(meter.sample(window: 2, presents: 90, at: 12) == 60)
    }

    @Test func withoutAWindowThereIsNoFrameRate() {
        var meter = FrameRateMeter()
        _ = meter.sample(window: 1, presents: 900, at: 10)

        #expect(meter.sample(window: 0, presents: 0, at: 11) == nil)
        #expect(meter.sample(window: 1, presents: 960, at: 12) == nil)
        #expect(meter.sample(window: 1, presents: 1020, at: 13) == 60)
    }

    /// `apkrt_window_presents` gives 0 once the window is removed, before the session learns of it.
    @Test func aCounterBackAtZeroIsNoFrameRate() {
        var meter = FrameRateMeter()
        _ = meter.sample(window: 1, presents: 900, at: 10)

        #expect(meter.sample(window: 1, presents: 0, at: 11) == nil)
        #expect(meter.sample(window: 1, presents: 0, at: 12) == 0)
    }

    @Test func aSampleWithoutTimePassingKeepsTheLastOne() {
        var meter = FrameRateMeter()
        _ = meter.sample(window: 1, presents: 0, at: 10)

        #expect(meter.sample(window: 1, presents: 5, at: 10) == nil)
        #expect(meter.sample(window: 1, presents: 60, at: 11) == 60)
    }
}
