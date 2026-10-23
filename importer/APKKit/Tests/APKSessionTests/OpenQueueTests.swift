import Foundation
import Testing

@testable import APKSession

/// ADR-0062: the host shows one open request at a time, answers each once, and never one that the guest cancelled.
@Suite("OpenQueue")
struct OpenQueueTests {
    static let page = "1\thttps%3A%2F%2Fexample.org"
    static let call = "1\ttel%3A123"
    static let pageURL = URL(string: "https://example.org")!
    static let callURL = URL(string: "tel:123")!

    @Test func aRequestIsShownAndAnsweredWhenItEnds() {
        var queue = OpenQueue()
        #expect(queue.requested(id: 1, text: Self.page) == [.present(id: 1, .browser(Self.pageURL))])
        #expect(queue.ended(id: 1, .shown) == [.answer(id: 1, .shown)])
        #expect(queue.ended(id: 1, .shown) == [], "answered once")
    }

    @Test func requestsWaitForTheOneShown() {
        var queue = OpenQueue()
        #expect(queue.requested(id: 1, text: Self.page) == [.present(id: 1, .browser(Self.pageURL))])
        #expect(queue.requested(id: 2, text: Self.call) == [])
        #expect(queue.ended(id: 1, .cancelled)
            == [.answer(id: 1, .cancelled), .present(id: 2, .systemOpen(Self.callURL))])
        #expect(queue.ended(id: 2, .noHandler) == [.answer(id: 2, .noHandler)])
    }

    @Test func aRefusedRequestIsAnsweredAtOnceWithoutTakingTurns() {
        var queue = OpenQueue()
        #expect(queue.requested(id: 1, text: Self.page) == [.present(id: 1, .browser(Self.pageURL))])
        #expect(queue.requested(id: 2, text: "1\tjavascript%3Aalert%281%29") == [.answer(id: 2, .refused)])
        #expect(queue.requested(id: 3, text: "garbage") == [.answer(id: 3, .refused)])
    }

    @Test func nothingIsShownFromTheBackground() {
        var queue = OpenQueue()
        #expect(queue.requested(id: 1, text: Self.page, inBackground: true) == [.answer(id: 1, .background)])
        #expect(queue.requested(id: 2, text: Self.page) == [.present(id: 2, .browser(Self.pageURL))])
    }

    @Test func aCancelDismissesTheShownRequestWithoutAnAnswer() {
        var queue = OpenQueue()
        _ = queue.requested(id: 1, text: Self.page)
        _ = queue.requested(id: 2, text: Self.call)
        #expect(queue.cancelled(id: 1) == [.dismiss(id: 1), .present(id: 2, .systemOpen(Self.callURL))])
        #expect(queue.ended(id: 1, .cancelled) == [], "the dismissed sheet's end is no answer")
        #expect(queue.ended(id: 2, .shown) == [.answer(id: 2, .shown)])
    }

    @Test func anIDCanComeAgainAfterItsCancel() {
        var queue = OpenQueue()
        _ = queue.requested(id: 5, text: Self.call)
        #expect(queue.cancelled(id: 5) == [.dismiss(id: 5)])
        #expect(queue.requested(id: 5, text: Self.page) == [.present(id: 5, .browser(Self.pageURL))])
        #expect(queue.ended(id: 5, .shown) == [.answer(id: 5, .shown)])
    }

    @Test func aCancelOfAWaitingRequestDropsIt() {
        var queue = OpenQueue()
        _ = queue.requested(id: 1, text: Self.page)
        _ = queue.requested(id: 2, text: Self.call)
        #expect(queue.cancelled(id: 2) == [])
        #expect(queue.ended(id: 1, .shown) == [.answer(id: 1, .shown)])
        #expect(queue.cancelled(id: 9) == [])
    }

    @Test func theComposersAreTakenWhenTheDeviceCanSend() {
        let mail = "2\tmailto%3Aa%40example.org"
        var queue = OpenQueue()
        #expect(queue.requested(id: 1, text: mail, canSendMail: true)
            == [.present(id: 1, .mailComposer(MailDraft(to: ["a@example.org"], cc: [], bcc: [], subject: "", body: "")))])
        _ = queue.ended(id: 1, .shown)
        #expect(queue.requested(id: 2, text: mail, canSendMail: false)
            == [.present(id: 2, .systemOpen(URL(string: "mailto:a@example.org")!))])
    }

    @Test func atMostFourWait() {
        var queue = OpenQueue()
        for id in UInt64(1)...4 { _ = queue.requested(id: id, text: Self.page) }
        #expect(queue.requested(id: 5, text: Self.page) == [.answer(id: 5, .refused)])
        #expect(queue.requested(id: 3, text: Self.page) == [], "an ID that waits is dropped, its answer still to come")
    }
}
