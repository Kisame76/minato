import Testing

@testable import APKSession

/// When the starting window goes (ADR-0076 decision 1): at the first frame of the starting activity's window, the
/// session's, or of an activity's own requested window; once.
@Suite struct StartingWindowWatchTests {
    @Test func itGoesAtTheFirstPresentOfTheSessionsWindowOnce() {
        var watch = StartingWindowWatch()
        var presents: [UInt64: UInt64] = [:]

        #expect(watch.check([0], presents: { presents[$0] ?? 0 }) == nil)
        #expect(watch.check([5], presents: { presents[$0] ?? 0 }) == nil)
        presents[5] = 1
        #expect(watch.check([5], presents: { presents[$0] ?? 0 }) == 5)
        #expect(watch.isGone)
        presents[5] = 2
        #expect(watch.check([5], presents: { presents[$0] ?? 0 }) == nil)
    }

    /// A floating starting activity or a translucent next one draws into a window of its own.
    @Test func anActivitysOwnWindowTakesItAwayToo() {
        var watch = StartingWindowWatch()
        let presents: [UInt64: UInt64] = [7: 1]

        #expect(watch.check([5, 7], presents: { presents[$0] ?? 0 }) == 7)
    }
}
