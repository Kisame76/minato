import Testing

@testable import APKSession

/// ADR-0032, decision 9: the FPS label's number, from the window's presents counted at the gate
/// (`apkrt_window_presents`), sampled about once a second.
@Suite("FrameRateMeter")
struct FrameRateMeterTests {
    /// One case per rule, each on a new meter: the samples (window, presents, time) and the frame rate each gives.
    static let runs: [(String, [(UInt64, UInt64, Double, Int?)])] = [
        ("the first sample has nothing to compare with", [(1, 500, 10, nil)]),
        ("the difference over one second is the frame rate", [(1, 500, 10, nil), (1, 620, 11, 120), (1, 680, 12, 60)]),
        ("about once a second: the difference is divided by the time that really passed",
         [(1, 0, 10, nil), (1, 66, 11.1, 60), (1, 125, 12.08, 60)]),
        ("the gate is closed (the menu, the app switcher): no presents is zero", [(1, 300, 10, nil), (1, 300, 11, 0)]),
        ("at background the window goes; the next one counts from its own start",
         [(1, 900, 10, nil), (2, 30, 11, nil), (2, 90, 12, 60)]),
        ("without a window there is no frame rate",
         [(1, 900, 10, nil), (0, 0, 11, nil), (1, 960, 12, nil), (1, 1020, 13, 60)]),
        ("apkrt_window_presents gives 0 once the window is removed, before the session learns of it",
         [(1, 900, 10, nil), (1, 0, 11, nil), (1, 0, 12, 0)]),
        ("a sample without time passing keeps the last one", [(1, 0, 10, nil), (1, 5, 10, nil), (1, 60, 11, 60)]),
    ]

    @Test(arguments: FrameRateMeterTests.runs)
    func theFrameRateIsThePresentsPerSecond(rule: String, samples: [(UInt64, UInt64, Double, Int?)]) {
        var meter = FrameRateMeter()
        for (window, presents, time, rate) in samples {
            #expect(meter.sample(window: window, presents: presents, at: time) == rate, "\(rule), at \(time)")
        }
    }
}
