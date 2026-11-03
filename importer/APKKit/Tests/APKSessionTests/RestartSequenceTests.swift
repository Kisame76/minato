import Testing

@testable import APKSession

/// ADR-0032, decision 2 as it was refined: the workspace first, which targets this install's bundle ID, then
/// the URL, which a second install may answer, then the fallback.
@Suite("RestartSequence")
struct RestartSequenceTests {
    @Test func theWorkspaceComesFirstThenTheURL() {
        var sequence = RestartSequence(requested: nil)

        #expect(sequence.start() == .attempt(.workspace))
        #expect(sequence.failed(.workspace) == .attempt(.ownURL))
        #expect(sequence.answered(.ownURL) == .exit)
    }

    @Test func anAnsweredWorkspaceEndsTheProcess() {
        var sequence = RestartSequence(requested: nil)
        _ = sequence.start()

        #expect(sequence.answered(.workspace) == .exit)
        #expect(sequence.failed(.workspace) == nil)
    }

    @Test func whenBothFailTheFallbackShows() {
        var sequence = RestartSequence(requested: nil)
        _ = sequence.start()
        _ = sequence.failed(.workspace)

        #expect(sequence.failed(.ownURL) == .fallback)
        #expect(sequence.answered(.ownURL) == nil)
    }

    /// session-check forces a way: only that one is tried.
    @Test func aRequestedWayIsTheOnlyOne() {
        for way in [RestartWay.ownURL, .workspace] {
            var sequence = RestartSequence(requested: way)

            #expect(sequence.start() == .attempt(way))
            #expect(sequence.failed(way) == .fallback)
        }
    }

    /// "none" is tried and fails at once, so its events stay as session-check knows them.
    @Test func noneIsAWayThatAlwaysFails() {
        var sequence = RestartSequence(requested: RestartWay.none)

        #expect(sequence.start() == .attempt(RestartWay.none))
        #expect(sequence.failed(RestartWay.none) == .fallback)
    }

    /// The workspace's answer comes after its timeout moved on to the URL: only the URL's answer counts.
    @Test func aLateAnswerOfAnEarlierWayChangesNothing() {
        var sequence = RestartSequence(requested: nil)
        _ = sequence.start()
        _ = sequence.failed(.workspace)

        #expect(sequence.answered(.workspace) == nil)
        #expect(sequence.failed(.workspace) == nil)
        #expect(sequence.answered(.ownURL) == .exit)
    }

    @Test func aSequenceStartsOnce() {
        var sequence = RestartSequence(requested: nil)
        _ = sequence.start()

        #expect(sequence.start() == nil)
    }

    @Test func theWaysKeepTheirNames() {
        #expect(RestartWay.allCases.map(\.rawValue) == ["url", "workspace", "none"])
    }
}
