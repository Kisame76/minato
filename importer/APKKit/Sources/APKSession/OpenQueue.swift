/// The guest's open requests as the host shows them (ADR-0062): one at a time, as iOS presents one sheet at a time,
/// in the order the runtime took them; each answered once with APKRT_EVENT_OPEN_RESULT, a refused or background one at
/// once, and a cancelled one never, as APKRT_REQUEST_OPEN_CANCEL asks.
public struct OpenQueue: Sendable {
    /// What the host does next.
    public enum Step: Sendable, Equatable {
        /// Show the request's sheet, composer or iOS's open.
        case present(id: UInt64, OpenAction)
        /// Post APKRT_EVENT_OPEN_RESULT.
        case answer(id: UInt64, OpenOutcome)
        /// Take the request's sheet away, without an answer.
        case dismiss(id: UInt64)
    }

    /// The runtime lets four requests wait at most.
    static let waitingLimit = 4

    private var shown: UInt64?
    private var waiting: [(id: UInt64, action: OpenAction)] = []

    public init() {}

    /// The runtime's request `id` with its text: shown when nothing else is, else after the ones before it.
    public mutating func requested(id: UInt64, text: String, inBackground: Bool = false, canSendMail: Bool = false,
                                   canSendText: Bool = false) -> [Step] {
        // An ID that waits already has its answer to come; the runtime refuses such a request first.
        guard shown != id, !waiting.contains(where: { $0.id == id }) else { return [] }
        guard !inBackground else { return [.answer(id: id, .background)] }
        guard waiting.count + (shown == nil ? 0 : 1) < Self.waitingLimit,
              let request = OpenRequest.parse(text)
        else { return [.answer(id: id, .refused)] }
        waiting.append((id, request.action(canSendMail: canSendMail, canSendText: canSendText)))
        return shown == nil ? next() : []
    }

    /// The shown request `id` ended with `outcome`; nothing for one that is not shown, such as a dismissed one.
    public mutating func ended(id: UInt64, _ outcome: OpenOutcome) -> [Step] {
        guard shown == id else { return [] }
        shown = nil
        return [.answer(id: id, outcome)] + next()
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
        return [.present(id: first.id, first.action)]
    }
}
