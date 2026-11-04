import Testing

@testable import APKLibrary

/// VoiceOver's actions for the manual order, as an alternative to dragging: Move Up, Move Down, Move to Top and Move
/// to Bottom. Each gives where Library.move(_:before:) puts the app and the app's new place, or nothing when the app
/// is there already.
@Suite("Library move steps")
struct LibraryMoveStepTests {
    private let ids = ["a", "b", "c", "d"]

    private func move(_ step: LibraryMoveStep, _ id: String) -> LibraryMove? {
        step.move(id, in: ids)
    }

    @Test func upGoesBeforeThePreviousApp() {
        #expect(move(.up, "c") == LibraryMove(before: "b", position: 2))
        #expect(move(.up, "b") == LibraryMove(before: "a", position: 1))
    }

    @Test func downGoesBeforeTheAppAfterTheNextOne() {
        #expect(move(.down, "a") == LibraryMove(before: "c", position: 2))
        #expect(move(.down, "c") == LibraryMove(before: nil, position: 4))
    }

    @Test func toTopGoesBeforeTheFirstAppAndToBottomToTheEnd() {
        #expect(move(.toTop, "c") == LibraryMove(before: "a", position: 1))
        #expect(move(.toBottom, "b") == LibraryMove(before: nil, position: 4))
    }

    @Test func anAppThatIsThereAlreadyOrUnknownMovesNowhere() {
        #expect(move(.up, "a") == nil)
        #expect(move(.toTop, "a") == nil)
        #expect(move(.down, "d") == nil)
        #expect(move(.toBottom, "d") == nil)
        #expect(move(.up, "x") == nil)
    }

    @Test func onlyTheAppsThatCanMoveOfferTheStep() {
        #expect(LibraryMoveStep.allCases.filter { move($0, "a") != nil } == [.down, .toBottom])
        #expect(LibraryMoveStep.allCases.filter { move($0, "d") != nil } == [.up, .toTop])
        #expect(LibraryMoveStep.allCases.filter { move($0, "b") != nil } == [.up, .down, .toTop, .toBottom])
    }
}
