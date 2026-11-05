import Foundation

/// A dangerous permission group the host decides (guest_interface.h, APKRT_PERMISSION_*).
public enum PermissionGroup: Int32, Sendable, Codable {
    case location = 1
    case microphone = 2
}

/// APKRT_REQUEST_PERMISSION's text, "<group>\t<detail>".
public struct PermissionAsk: Sendable, Equatable {
    public var group: PermissionGroup
    /// APKRT_PERMISSION_DETAIL_*: for location 1 fine, 2 coarse only; 0 for the microphone.
    public var detail: Int32

    public init(group: PermissionGroup, detail: Int32) {
        self.group = group
        self.detail = detail
    }

    public init?(text: String) {
        let fields = text.split(separator: "\t", omittingEmptySubsequences: false)
        guard fields.count == 2, let number = Int32(fields[0]), let group = PermissionGroup(rawValue: number),
              let detail = Int32(fields[1]) else { return nil }
        self.init(group: group, detail: detail)
    }
}

/// What the host answered an app for a group, kept across its sessions: the outcome (APKRT_PERMISSION_*) and the
/// denials in a row, of which the second is Android's "don't ask again".
public struct PermissionRecord: Sendable, Equatable, Codable {
    public var outcome: Int32
    public var denials: Int

    public init(outcome: Int32 = 0, denials: Int = 0) {
        self.outcome = outcome
        self.denials = denials
    }
}

/// An app's records, in a file of the host's beside the folders the guest sees (LibraryPaths.permissionsFile), so the
/// app cannot write them.
public struct PermissionRecords: Sendable, Equatable, Codable {
    public var microphone = PermissionRecord()

    public init(microphone: PermissionRecord = PermissionRecord()) {
        self.microphone = microphone
    }

    /// The records in `file`; none for a missing or unreadable one.
    public static func load(from file: URL) -> PermissionRecords {
        guard let data = try? Data(contentsOf: file),
              let records = try? JSONDecoder().decode(PermissionRecords.self, from: data)
        else { return PermissionRecords() }
        return records
    }

    public func save(to file: URL) throws {
        try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        try JSONEncoder().encode(self).write(to: file, options: .atomic)
    }
}

/// The host's side of APKRT_REQUEST_PERMISSION (ADR-0063 decision 2, ADR-0064 decision 2): one request at a time, in
/// the runtime's order. iOS's answer is Minato's, for every app, so Minato asks in an alert of its own that names the
/// app first, and only a yes there goes on to iOS's prompt, and only while iOS has not answered; a no never reaches
/// iOS. The answer is kept per app; a grant clears the denials, and a request after the second denial in a row is
/// answered no at once, as Android stops showing its dialog then. When iOS has denied, the answer is no without the
/// alert. Location is answered no at once until its host comes. The start seeds a stored grant iOS still gives, and
/// each return to the foreground answers a grant that iOS no longer gives as a denial (request ID 0, not counted).
public struct PermissionDesk: Sendable {
    /// iOS's answer for the group's own permission (AVAudioApplication.recordPermission for the microphone).
    public enum IOSState: Sendable {
        case notDetermined, granted, denied
    }

    public enum Step: Sendable, Equatable {
        /// apkrt_permission_answer(id, group, outcome).
        case answer(id: UInt64, group: Int32, outcome: Int32)
        /// Show Minato's alert for the request.
        case ask(id: UInt64, group: PermissionGroup)
        /// Ask iOS (its prompt), then iosAnswered.
        case askIOS(id: UInt64, group: PermissionGroup)
        /// Take the request's alert away, without an answer.
        case dismiss(id: UInt64)
    }

    public private(set) var records: PermissionRecords
    private var shown: (id: UInt64, group: PermissionGroup)?
    private var waiting: [(id: UInt64, group: PermissionGroup)] = []

    public init(records: PermissionRecords) {
        self.records = records
    }

    /// The runtime's request `id` with its text, at iOS's answer `ios`.
    public mutating func requested(id: UInt64, text: String, ios: IOSState) -> [Step] {
        guard shown?.id != id, !waiting.contains(where: { $0.id == id }) else { return [] }
        guard let ask = PermissionAsk(text: text) else { return [.answer(id: id, group: 0, outcome: 0)] }
        guard ask.group == .microphone else { return [.answer(id: id, group: ask.group.rawValue, outcome: 0)] }
        waiting.append((id, ask.group))
        return shown == nil ? next(ios: ios) : []
    }

    /// The user allowed the shown request `id` in Minato's alert.
    public mutating func allowed(id: UInt64, ios: IOSState) -> [Step] {
        guard let shown, shown.id == id else { return [] }
        switch ios {
        case .notDetermined: return [.askIOS(id: id, group: shown.group)]
        case .granted: return finish(granted: true, ios: ios)
        case .denied: return finish(granted: false, ios: ios)
        }
    }

    /// The user said no in Minato's alert for the shown request `id`.
    public mutating func denied(id: UInt64) -> [Step] {
        guard let shown, shown.id == id else { return [] }
        return finish(granted: false, ios: .notDetermined)
    }

    /// iOS's prompt for the shown request `id` was answered.
    public mutating func iosAnswered(id: UInt64, granted: Bool) -> [Step] {
        guard let shown, shown.id == id else { return [] }
        return finish(granted: granted, ios: granted ? .granted : .denied)
    }

    /// The guest no longer wants request `id`: a shown one is dismissed, a waiting one dropped, neither answered.
    public mutating func cancelled(id: UInt64, ios: IOSState = .notDetermined) -> [Step] {
        if shown?.id == id {
            shown = nil
            return [.dismiss(id: id)] + next(ios: ios)
        }
        waiting.removeAll { $0.id == id }
        return []
    }

    /// At the session's start: the stored grant, when iOS still gives it.
    public func seeds(ios: IOSState) -> [Step] {
        records.microphone.outcome == 1 && ios == .granted ? [.answer(id: 0, group: 2, outcome: 1)] : []
    }

    /// At each return to the foreground: a stored grant that iOS no longer gives becomes a denial, not counted.
    public mutating func foreground(ios: IOSState) -> [Step] {
        guard records.microphone.outcome == 1, ios != .granted else { return [] }
        records.microphone.outcome = 0
        return [.answer(id: 0, group: 2, outcome: 0)]
    }

    private mutating func finish(granted: Bool, ios: IOSState) -> [Step] {
        guard let shown else { return [] }
        self.shown = nil
        return [answer(shown.id, granted: granted)] + next(ios: ios)
    }

    /// The answer of a request, which the record keeps.
    private mutating func answer(_ id: UInt64, granted: Bool) -> Step {
        if granted {
            records.microphone = PermissionRecord(outcome: 1, denials: 0)
        } else {
            records.microphone.outcome = 0
            records.microphone.denials += 1
        }
        return .answer(id: id, group: PermissionGroup.microphone.rawValue, outcome: granted ? 1 : 0)
    }

    /// The next waiting request: answered at once from the record or iOS's denial, or shown.
    private mutating func next(ios: IOSState) -> [Step] {
        var steps: [Step] = []
        while !waiting.isEmpty {
            let first = waiting.removeFirst()
            if records.microphone.outcome == 1 && ios == .granted {
                steps.append(.answer(id: first.id, group: first.group.rawValue, outcome: 1))
            } else if records.microphone.denials >= 2 {
                steps.append(.answer(id: first.id, group: first.group.rawValue, outcome: 0))
            } else if ios == .denied {
                steps.append(answer(first.id, granted: false))
            } else {
                shown = first
                steps.append(.ask(id: first.id, group: first.group))
                break
            }
        }
        return steps
    }
}
