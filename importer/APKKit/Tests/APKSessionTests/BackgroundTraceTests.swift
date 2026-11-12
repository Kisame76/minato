import Testing

@testable import APKSession

/// Minato's own session-log lines about a trip to the background: when the guest answered its pause and its stop,
/// and the memory before the trip and 10 s after the return; counts and times
/// only, no app name and no guest text.
@Suite("BackgroundTrace")
struct BackgroundTraceTests {
    static let second: UInt64 = 1_000_000_000
    static let summary = "foot=812 gl tex=5/2 priv=1 shared=1 comp=1/0 color=2/1 depth=1/0 msaa=1/1 mless=0 buf=2/2"

    @Test func aTripNumbersItselfAndSaysTheMemoryBefore() {
        var trace = BackgroundTrace()
        #expect(trace.entered(at: 100 * Self.second, memory: Self.summary)
            == "I/minato   : trip 1 to the background: foot=812 tex=5/2 depth=1/0")
        #expect(trace.trip == 1 && trace.enteredAt == 100 * Self.second)
        #expect(trace.returned(at: 115 * Self.second, gateParked: 0, gateLongest: 0) == 1)
        #expect(trace.entered(at: 200 * Self.second, memory: "foot=900").hasPrefix("I/minato   : trip 2 "))
    }

    @Test func onlyAPauseOrStopOfATripIsTimed() {
        var trace = BackgroundTrace()
        #expect(!trace.times(.onPause))
        _ = trace.entered(at: Self.second, memory: Self.summary)
        #expect(trace.times(.onPause) && trace.times(.onStop))
        #expect(!trace.times(.onSaveInstanceState) && !trace.times(.onNativeWindowDestroyed))
        _ = trace.returned(at: 2 * Self.second, gateParked: 0, gateLongest: 0)
        #expect(!trace.times(.onStop))
    }

    @Test func anAnswerSaysItsTimeAfterTheBackground() {
        let entered = 100 * Self.second
        #expect(BackgroundTrace.answerLine(trip: 3, callback: .onPause, result: 1, at: entered + 120_400_000,
                                           enteredAt: entered)
            == "I/minato   : trip 3 pause answered 120 ms after the background")
        #expect(BackgroundTrace.answerLine(trip: 3, callback: .onStop, result: 1, at: entered + 8 * Self.second,
                                           enteredAt: entered)
            == "I/minato   : trip 3 stop answered 8000 ms after the background")
        #expect(BackgroundTrace.answerLine(trip: 1, callback: .onStop, result: 0, at: entered + 600 * Self.second,
                                           enteredAt: entered)
            == "I/minato   : trip 1 stop not answered 600000 ms after the background")
        #expect(BackgroundTrace.answerLine(trip: 1, callback: .onPause, result: -1, at: entered + 5 * Self.second,
                                           enteredAt: entered)
            == "I/minato   : trip 1 pause unanswered, the session ended 5000 ms after the background")
    }

    @Test func theReturnLineHasTheMemoryBeforeAndTenSecondsAfter() {
        var trace = BackgroundTrace()
        _ = trace.entered(at: 100 * Self.second, memory: Self.summary)
        #expect(trace.returned(at: 115_250_000_000, gateParked: 3, gateLongest: 14_600_000_000) == 1)
        #expect(trace.returnLine(trip: 1, after: "foot=1020 gl tex=9/3 priv=0 depth=2/0")
            == "I/minato   : trip 1 back after 15.2 s: foot=812 tex=5/2 depth=1/0 before, "
            + "foot=1020 tex=9/3 depth=2/0 10 s after; commit gate: 3 parked, longest 14600 ms")
        // Another trip came meanwhile: the old one's line is not written.
        _ = trace.entered(at: 120 * Self.second, memory: Self.summary)
        #expect(trace.returnLine(trip: 1, after: "foot=1") == nil)
    }

    @Test func aReturnWithoutATripIsNone() {
        var trace = BackgroundTrace()
        #expect(trace.returned(at: Self.second, gateParked: 0, gateLongest: 0) == nil)
        _ = trace.entered(at: Self.second, memory: "foot=1")
        #expect(trace.returned(at: 2 * Self.second, gateParked: 0, gateLongest: 0) == 1)
        #expect(trace.returned(at: 3 * Self.second, gateParked: 0, gateLongest: 0) == nil)
    }

    @Test func theMemoryKeepsOnlyTheFootprintTexturesAndDepth() {
        #expect(BackgroundTrace.memory(Self.summary) == "foot=812 tex=5/2 depth=1/0")
        #expect(BackgroundTrace.memory("foot=40") == "foot=40")
        #expect(BackgroundTrace.memory("") == "foot=?")
    }
}
