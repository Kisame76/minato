import Synchronization
import Testing

@testable import APKSession

/// ADR-0057: the guest's pick requests as the iPhone's host shows them, one at a time, each answered once and a
/// cancelled one never; and the one turn the pickers and ADR-0062's sheets share over game mode.
@Suite("PickQueue")
struct PickQueueTests {
    static let document = "1\t0\t0\ttext/plain\t"
    static let request = PickRequest(text: document)!

    @Test func aDocumentRequestIsShownAndAnsweredOnce() {
        var queue = PickQueue()
        #expect(queue.requested(id: 1, text: Self.document) == [.present(id: 1, Self.request)])
        #expect(queue.ended(id: 1, picked: 2) == [.answer(id: 1, picked: 2)])
        #expect(queue.ended(id: 1, picked: 2) == [])
    }

    @Test func theNextWaitsForTheOneShown() {
        var queue = PickQueue()
        _ = queue.requested(id: 1, text: Self.document)
        #expect(queue.requested(id: 2, text: Self.document) == [])
        #expect(queue.ended(id: 1, picked: nil) == [.answer(id: 1, picked: nil), .present(id: 2, Self.request)])
    }

    @Test func thePhotosWithoutAViewAndABadTextAreCancelledAtOnce() {
        var queue = PickQueue()
        for (id, text) in [(3, "5\t1\t20\timage/*\t"), (4, "nonsense")] as [(UInt64, String)] {
            #expect(queue.requested(id: id, text: text) == [.answer(id: id, picked: nil)])
        }
    }

    /// Step d: a save and a folder are shown as the documents are, one after the other.
    @Test func aSaveAndAFolderAreShown() {
        var queue = PickQueue()
        let save = "3\t0\t0\ttext/plain\tnote.txt", tree = "4\t0\t0\t\t"
        #expect(queue.requested(id: 1, text: save) == [.present(id: 1, PickRequest(text: save)!)])
        #expect(queue.requested(id: 2, text: tree) == [])
        #expect(queue.ended(id: 1, picked: 1) == [.answer(id: 1, picked: 1), .present(id: 2, PickRequest(text: tree)!)])
    }

    @Test func aRequestInTheBackgroundIsCancelledAtOnce() {
        var queue = PickQueue()
        #expect(queue.requested(id: 1, text: Self.document, inBackground: true) == [.answer(id: 1, picked: nil)])
    }

    @Test func aCancelDismissesTheShownOneOrDropsAWaitingOneWithoutAnAnswer() {
        var queue = PickQueue()
        _ = queue.requested(id: 1, text: Self.document)
        _ = queue.requested(id: 2, text: Self.document)
        _ = queue.requested(id: 3, text: Self.document)
        #expect(queue.cancelled(id: 2) == [])
        #expect(queue.cancelled(id: 1) == [.dismiss(id: 1), .present(id: 3, Self.request)])
        #expect(queue.ended(id: 1, picked: 1) == [])  // a late answer of the dismissed one
        #expect(queue.ended(id: 3, picked: 1) == [.answer(id: 3, picked: 1)])
    }

    @Test func anIDThatWaitsIsNotTakenTwice() {
        var queue = PickQueue()
        _ = queue.requested(id: 1, text: Self.document)
        _ = queue.requested(id: 2, text: Self.document)
        #expect(queue.requested(id: 1, text: Self.document) == [])
        #expect(queue.requested(id: 2, text: Self.document) == [])
        #expect(queue.ended(id: 1, picked: nil) == [.answer(id: 1, picked: nil), .present(id: 2, Self.request)])
        #expect(queue.ended(id: 2, picked: nil) == [.answer(id: 2, picked: nil)])
    }

    /// The pickers' and the open requests' sheets take turns: a cancel of one dismisses only its own.
    @MainActor @Test func sheetsTakeTurns() {
        final class Owner {}
        let turns = SheetTurns()
        let pick = Owner(), open = Owner(), other = Owner()
        var started: [String] = []
        turns.take(open) { started.append("open") }
        turns.take(pick) { started.append("pick") }
        turns.take(other) { started.append("other") }
        #expect(started == ["open"])
        turns.give(other)  // gave up while it waited
        turns.give(pick)  // so did this one
        #expect(started == ["open"])
        turns.take(pick) { started.append("pick again") }
        turns.give(open)
        #expect(started == ["open", "pick again"])
        turns.give(open)  // not its turn: nothing
        #expect(started == ["open", "pick again"])
        turns.give(pick)
        turns.take(open) { started.append("open again") }
        #expect(started == ["open", "pick again", "open again"])
    }

    /// The checks SheetSteps schedules, run by hand.
    @MainActor final class Clock {
        var due: [() -> Void] = []
        func after(_ seconds: Double, _ work: @escaping @MainActor () -> Void) { due.append(work) }
        /// Runs what is due now, which may schedule the next.
        func tick() {
            let now = due
            due = []
            now.forEach { $0() }
        }
    }

    /// A presenter's turn goes only once its dismissed sheet is gone: the next owner presented over a sheet on its way
    /// out would go with it, never answered, and keep the turn (the review); the steps that come meanwhile
    /// wait as well, and a check after the callback changes nothing.
    @MainActor @Test func theTurnGoesOnceTheDismissedSheetIsGone() {
        final class Owner {}
        let turns = SheetTurns(), clock = Clock()
        let pick = Owner(), open = Owner()
        var started: [String] = [], ran: [String] = []
        var gone: (() -> Void)?
        turns.take(pick) {}
        turns.take(open) { started.append("open") }
        let steps = SheetSteps<String>(owner: pick, turns: turns, after: clock.after) { step in
            ran.append(step)
            return step == "dismiss" ? .gone(.init(start: { gone = $0 }, isGone: { false })) : .next
        }
        steps.run(["dismiss", "after"])
        steps.run(["meanwhile"])
        #expect(ran == ["dismiss"])
        #expect(started == [])
        gone?()
        #expect(started == ["open"])
        #expect(ran == ["dismiss", "after", "meanwhile"])
        clock.tick()
        #expect(clock.due.isEmpty)  // no checks after the end
        gone?()
        #expect(started == ["open"])
        #expect(ran == ["dismiss", "after", "meanwhile"])
    }

    /// A late callback of an earlier dismissal does not end the next one.
    @MainActor @Test func aLateCallbackDoesNotEndTheNextDismissal() {
        final class Owner {}
        let turns = SheetTurns(), clock = Clock()
        let pick = Owner(), open = Owner()
        var started: [String] = []
        var callbacks: [() -> Void] = []
        turns.take(pick) {}
        let steps = SheetSteps<String>(owner: pick, turns: turns, after: clock.after) { step in
            step == "dismiss" ? .gone(.init(start: { callbacks.append($0) }, isGone: { false })) : .next
        }
        steps.run(["dismiss"])
        callbacks[0]()
        turns.take(pick) {}
        turns.take(open) { started.append("open") }
        steps.run(["dismiss"])
        callbacks[0]()
        #expect(started == [])
        callbacks[1]()
        #expect(started == ["open"])
    }

    /// UIKit drops a dismiss sent while a transition runs and never calls back (the reviewer): the sheet seen gone
    /// gives the turn all the same.
    @MainActor @Test func aSheetSeenGoneGivesTheTurnWithoutTheCallback() {
        final class Owner {}
        let turns = SheetTurns(), clock = Clock()
        let pick = Owner(), open = Owner()
        var started: [String] = [], ran: [String] = []
        var isGone = false
        turns.take(pick) {}
        turns.take(open) { started.append("open") }
        let steps = SheetSteps<String>(owner: pick, turns: turns, after: clock.after) { step in
            ran.append(step)
            return step == "dismiss" ? .gone(.init(start: { _ in }, isGone: { isGone })) : .next
        }
        steps.run(["dismiss", "after"])
        clock.tick()
        #expect(started == [])
        isGone = true
        clock.tick()
        #expect(started == ["open"])
        #expect(ran == ["dismiss", "after"])
    }

    /// A sheet that stays is asked to go again each second, and after 5 s the turn goes all the same, so that no
    /// request waits behind it for good.
    @MainActor @Test func aSheetThatStaysIsDismissedAgainAndTheTurnGoesAfterFiveSeconds() {
        final class Owner {}
        let turns = SheetTurns(), clock = Clock()
        let pick = Owner(), open = Owner()
        var started: [String] = []
        var dismissals = 0
        turns.take(pick) {}
        turns.take(open) { started.append("open") }
        let steps = SheetSteps<String>(owner: pick, turns: turns, after: clock.after) { step in
            step == "dismiss" ? .gone(.init(start: { _ in dismissals += 1 }, isGone: { false })) : .next
        }
        steps.run(["dismiss"])
        for _ in 0..<49 { clock.tick() }  // 4.9 s
        #expect(started == [])
        #expect(dismissals == 5)
        clock.tick()
        #expect(started == ["open"])
        #expect(clock.due.isEmpty)
    }

    /// Without a sheet to take away, the turn goes at once.
    @MainActor @Test func withoutASheetTheTurnGoesAtOnce() {
        final class Owner {}
        let turns = SheetTurns()
        let pick = Owner(), open = Owner()
        var started: [String] = [], ran: [String] = []
        turns.take(pick) {}
        turns.take(open) { started.append("open") }
        let steps = SheetSteps<String>(owner: pick, turns: turns, after: Clock().after) { step in
            ran.append(step)
            return step == "dismiss" ? .gone(nil) : .next
        }
        steps.run(["dismiss", "after"])
        #expect(started == ["open"])
        #expect(ran == ["dismiss", "after"])
    }

    /// One presenter's clone work runs one job after the other, each to its end: a cancelled request's never beside
    /// the next one's (the review).
    @Test func serialWorkRunsOneJobAfterTheOther() async {
        let work = SerialWork()
        let log = Mutex<[String]>([])
        work.run {
            log.withLock { $0.append("first begins") }
            try? await Task.sleep(for: .milliseconds(100))
            log.withLock { $0.append("first ends") }
        }
        work.run { log.withLock { $0.append("second") } }
        await work.idle()
        #expect(log.withLock { $0 } == ["first begins", "first ends", "second"])
    }
}
