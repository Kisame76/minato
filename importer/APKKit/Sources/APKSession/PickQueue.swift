import Foundation
import Synchronization

/// The guest's pick requests as the iPhone's host shows them (ADR-0057): one at a time, in the order the runtime took
/// them; each answered once with APKRT_EVENT_PICK_RESULT, and a cancelled one never, as APKRT_REQUEST_PICK_CANCEL
/// asks. A request in the background, one of a kind without a view yet (3 to 5) and a text the host cannot read are
/// answered as cancelled at once.
public struct PickQueue: Sendable {
    /// What the host does next.
    public enum Step: Sendable, Equatable {
        /// Show the request's picker.
        case present(id: UInt64, PickRequest)
        /// Post APKRT_EVENT_PICK_RESULT: the number of picks the manifest names, nil for RESULT_CANCELED.
        case answer(id: UInt64, picked: Int?)
        /// Take the request's picker away, without an answer.
        case dismiss(id: UInt64)
    }

    private var shown: UInt64?
    private var waiting: [(id: UInt64, request: PickRequest)] = []

    public init() {}

    /// The runtime's request `id` with its text: shown when nothing else is, else after the ones before it.
    public mutating func requested(id: UInt64, text: String, inBackground: Bool = false) -> [Step] {
        // An ID that waits already has its answer to come; the runtime refuses such a request first.
        guard shown != id, !waiting.contains(where: { $0.id == id }) else { return [] }
        guard !inBackground, let request = PickRequest(text: text),
              request.kind == .openDocument || request.kind == .getContent
        else { return [.answer(id: id, picked: nil)] }
        waiting.append((id, request))
        return shown == nil ? next() : []
    }

    /// The shown request `id` ended with `picked` files, nil for a cancel; nothing for one that is not shown.
    public mutating func ended(id: UInt64, picked: Int?) -> [Step] {
        guard shown == id else { return [] }
        shown = nil
        return [.answer(id: id, picked: picked)] + next()
    }

    /// The guest no longer wants request `id`: a shown one is dismissed, a waiting one dropped, neither answered.
    public mutating func cancelled(id: UInt64) -> [Step] {
        if shown == id {
            shown = nil
            return [.dismiss(id: id)] + next()
        }
        waiting.removeAll { $0.id == id }
        return []
    }

    private mutating func next() -> [Step] {
        guard !waiting.isEmpty else { return [] }
        let first = waiting.removeFirst()
        shown = first.id
        return [.present(id: first.id, first.request)]
    }
}

/// The one turn over game mode that the pickers (ADR-0057) and the open requests' sheets (ADR-0062) take in order:
/// iOS shows one sheet at a time, and a presenter that dismissed its sheet through the controller below it would take
/// the other's away too, without a word to its delegate.
@MainActor
public final class SheetTurns {
    public static let shared = SheetTurns()

    private var holder: ObjectIdentifier?
    private var waiting: [(owner: ObjectIdentifier, start: () -> Void)] = []

    public init() {}

    /// Runs `start` once `owner` has the turn: at once when no other holds it, else after those that asked before.
    public func take(_ owner: AnyObject, start: @escaping () -> Void) {
        let id = ObjectIdentifier(owner)
        guard holder == nil || holder == id else { return waiting.append((id, start)) }
        holder = id
        start()
    }

    /// `owner` is done: its turn goes to the next, and what it still waited for is dropped.
    public func give(_ owner: AnyObject) {
        let id = ObjectIdentifier(owner)
        waiting.removeAll { $0.owner == id }
        guard holder == id else { return }
        holder = nil
        guard !waiting.isEmpty else { return }
        let next = waiting.removeFirst()
        holder = next.owner
        next.start()
    }
}

/// A presenter's steps over game mode, run in order (ADR-0057, ADR-0062): while a sheet of its own is on its way out,
/// the steps that come wait, and its turn (SheetTurns) goes only once the sheet is gone, as the next owner would
/// present over it otherwise and go with it, never answered, keeping the turn. UIKit drops a dismiss sent while a
/// transition runs and never calls back, so the sheet is also looked at every 0.1 s and asked to go again each second;
/// after 5 s its turn goes all the same, so that no request waits behind it for good.
@MainActor
public final class SheetSteps<Step> {
    /// A sheet's way out.
    public struct Dismissal {
        /// Takes the sheet away and calls back once it is gone.
        let start: (@escaping () -> Void) -> Void
        /// Whether the sheet is gone.
        let isGone: () -> Bool

        public init(start: @escaping (@escaping () -> Void) -> Void, isGone: @escaping () -> Bool) {
            self.start = start
            self.isGone = isGone
        }
    }

    /// What a step did.
    public enum Outcome {
        /// The next step runs.
        case next
        /// The owner's sheet goes (nil when there is none to take away): its turn goes once it is gone, and the next
        /// step runs after.
        case gone(Dismissal?)
    }

    static var interval: Double { 0.1 }
    static var retryTicks: Int { 10 }
    static var limitTicks: Int { 50 }

    private weak var owner: AnyObject?
    private let turns: SheetTurns
    private let after: (Double, @escaping @MainActor () -> Void) -> Void
    private let perform: (Step) -> Outcome
    private var waiting: [Step] = []
    /// The dismissal under way, by number, so that its callback and its checks count once.
    private var leaving: Int?
    private var dismissals = 0

    public convenience init(owner: AnyObject, perform: @escaping (Step) -> Outcome) {
        self.init(owner: owner, turns: .shared, after: { seconds, work in
            DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { MainActor.assumeIsolated(work) }
        }, perform: perform)
    }

    init(owner: AnyObject, turns: SheetTurns, after: @escaping (Double, @escaping @MainActor () -> Void) -> Void,
         perform: @escaping (Step) -> Outcome) {
        self.owner = owner
        self.turns = turns
        self.after = after
        self.perform = perform
    }

    public func run(_ steps: [Step]) {
        waiting += steps
        while leaving == nil, !waiting.isEmpty {
            guard case .gone(let dismissal) = perform(waiting.removeFirst()) else { continue }
            guard let dismissal else {
                giveTurn()
                continue
            }
            dismissals += 1
            let number = dismissals
            leaving = number
            dismissal.start { [weak self] in self?.left(number) }
            check(dismissal, number, tick: 1)
        }
    }

    private func check(_ dismissal: Dismissal, _ number: Int, tick: Int) {
        after(Self.interval) { [weak self] in
            guard let self, self.leaving == number else { return }
            if dismissal.isGone() || tick >= Self.limitTicks { return self.left(number) }
            if tick % Self.retryTicks == 0 { dismissal.start { [weak self] in self?.left(number) } }
            self.check(dismissal, number, tick: tick + 1)
        }
    }

    /// Dismissal `number` is over: the turn goes, then the steps that waited run.
    private func left(_ number: Int) {
        guard leaving == number else { return }
        leaving = nil
        giveTurn()
        run([])
    }

    private func giveTurn() {
        if let owner { turns.give(owner) }
    }
}

/// Work that runs one job after the other, each to its end, in the order given: a presenter's clones, so that a
/// cancelled request's never run beside the next one's (ADR-0057, the lead's reviewer).
public final class SerialWork: Sendable {
    private let last = Mutex<Task<Void, Never>?>(nil)

    public init() {}

    public func run(_ job: @escaping @Sendable () async -> Void) {
        last.withLock { last in
            let previous = last
            last = Task.detached(priority: .userInitiated) {
                await previous?.value
                await job()
            }
        }
    }

    /// Returns once every job given so far has ended.
    public func idle() async {
        await last.withLock { $0 }?.value
    }
}
