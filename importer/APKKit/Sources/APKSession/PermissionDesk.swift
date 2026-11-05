import Foundation

/// A dangerous permission group the host decides (guest_interface.h, APKRT_PERMISSION_*).
public enum PermissionGroup: Int32, Sendable, Codable, CaseIterable {
    case location = 1
    case microphone = 2
    /// ADR-0073 decision 8.
    case camera = 3
    /// ADR-0074 decision 1: POST_NOTIFICATIONS.
    case notifications = 4

    /// The order the start's seeds and the foreground's re-reads go in.
    static let answered: [PermissionGroup] = [.microphone, .location, .camera, .notifications]
}

/// APKRT_REQUEST_PERMISSION's text, "<group>\t<detail>".
public struct PermissionAsk: Sendable, Equatable {
    public var group: PermissionGroup
    /// APKRT_PERMISSION_DETAIL_*: for location 1 fine, 2 coarse only; 0 for the other groups.
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

/// What the host answered an app for a group, kept across its sessions: the outcome (APKRT_PERMISSION_*: 0 denied, 1
/// granted, for location fine, 2 location coarse) and the denials in a row, of which the second is Android's "don't
/// ask again".
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
    public var location = PermissionRecord()
    public var camera = PermissionRecord()
    public var notifications = PermissionRecord()

    public init(microphone: PermissionRecord = PermissionRecord(), location: PermissionRecord = PermissionRecord(),
                camera: PermissionRecord = PermissionRecord(), notifications: PermissionRecord = PermissionRecord()) {
        self.microphone = microphone
        self.location = location
        self.camera = camera
        self.notifications = notifications
    }

    /// A file of an older Minato may lack a group.
    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        microphone = try container.decodeIfPresent(PermissionRecord.self, forKey: .microphone) ?? PermissionRecord()
        location = try container.decodeIfPresent(PermissionRecord.self, forKey: .location) ?? PermissionRecord()
        camera = try container.decodeIfPresent(PermissionRecord.self, forKey: .camera) ?? PermissionRecord()
        notifications = try container.decodeIfPresent(PermissionRecord.self, forKey: .notifications)
            ?? PermissionRecord()
    }

    subscript(group: PermissionGroup) -> PermissionRecord {
        get {
            switch group {
            case .microphone: microphone
            case .location: location
            case .camera: camera
            case .notifications: notifications
            }
        }
        set {
            switch group {
            case .microphone: microphone = newValue
            case .location: location = newValue
            case .camera: camera = newValue
            case .notifications: notifications = newValue
            }
        }
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
/// iOS. The answer is kept per app and group; a grant clears the denials, and a request after the second denial in a
/// row is answered no at once, as Android stops showing its dialog then. When iOS has denied, the answer is no without
/// the alert and without a count, since that no is the user's in iOS's Settings, not the app's. Location is fine for a
/// request of ACCESS_FINE_LOCATION under iOS's full accuracy and coarse for a coarse-only request or under iOS's
/// reduced accuracy (decision 3); a stored coarse grant asks again for fine. The start seeds a stored grant iOS still
/// gives, and each return to the foreground answers a change of iOS's answer (request ID 0, not counted): a revoke as a
/// denial, reduced accuracy as coarse.
public struct PermissionDesk: Sendable {
    /// iOS's answer for the group's own permission (AVAudioApplication.recordPermission for the microphone,
    /// CLLocationManager's authorization and accuracy for location, AVCaptureDevice's for video for the camera,
    /// UNUserNotificationCenter's authorization for notifications).
    public enum IOSState: Sendable {
        case notDetermined, granted, denied
        /// Location authorized with reduced accuracy (precise location off).
        case grantedReduced

        var grants: Bool { self == .granted || self == .grantedReduced }
    }

    public typealias IOSStates = (PermissionGroup) -> IOSState

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

    private struct Request {
        var id: UInt64
        var ask: PermissionAsk
    }

    public private(set) var records: PermissionRecords
    private var shown: Request?
    private var waiting: [Request] = []

    public init(records: PermissionRecords) {
        self.records = records
    }

    /// The runtime's request `id` with its text, at iOS's answers `ios`.
    public mutating func requested(id: UInt64, text: String, ios: IOSStates) -> [Step] {
        guard shown?.id != id, !waiting.contains(where: { $0.id == id }) else { return [] }
        guard let ask = PermissionAsk(text: text) else { return [.answer(id: id, group: 0, outcome: 0)] }
        waiting.append(Request(id: id, ask: ask))
        return shown == nil ? next(ios: ios) : []
    }

    /// The user allowed the shown request `id` in Minato's alert.
    public mutating func allowed(id: UInt64, ios: IOSStates) -> [Step] {
        guard let shown, shown.id == id else { return [] }
        let state = ios(shown.ask.group)
        return state == .notDetermined ? [.askIOS(id: id, group: shown.ask.group)] : finish(state, ios: ios)
    }

    /// The user said no in Minato's alert for the shown request `id`.
    public mutating func denied(id: UInt64, ios: IOSStates = { _ in .notDetermined }) -> [Step] {
        guard let shown, shown.id == id else { return [] }
        return finish(.denied, ios: ios)
    }

    /// iOS's prompt for the shown request `id` was answered with `state`.
    public mutating func iosAnswered(id: UInt64, state: IOSState, ios: IOSStates? = nil) -> [Step] {
        guard let shown, shown.id == id else { return [] }
        let group = shown.ask.group
        return finish(state, ios: ios ?? { $0 == group ? state : .notDetermined })
    }

    /// The guest no longer wants request `id`: a shown one is dismissed, a waiting one dropped, neither answered.
    public mutating func cancelled(id: UInt64, ios: IOSStates = { _ in .notDetermined }) -> [Step] {
        if shown?.id == id {
            shown = nil
            return [.dismiss(id: id)] + next(ios: ios)
        }
        waiting.removeAll { $0.id == id }
        return []
    }

    /// At the session's start: each stored grant iOS still gives, coarse under iOS's reduced accuracy.
    public func seeds(ios: IOSStates) -> [Step] {
        PermissionGroup.answered.compactMap { group in
            let stored = records[group].outcome
            guard stored != 0, ios(group).grants else { return nil }
            return .answer(id: 0, group: group.rawValue, outcome: Self.adjusted(stored, group: group, ios(group)))
        }
    }

    /// At each return to the foreground: a stored grant that iOS no longer gives becomes a denial, a fine one under
    /// iOS's reduced accuracy coarse; neither is counted as a denial.
    public mutating func foreground(ios: IOSStates) -> [Step] {
        var steps: [Step] = []
        for group in PermissionGroup.answered {
            let stored = records[group].outcome
            guard stored != 0 else { continue }
            let now = ios(group).grants ? Self.adjusted(stored, group: group, ios(group)) : 0
            guard now != stored else { continue }
            records[group].outcome = now
            steps.append(.answer(id: 0, group: group.rawValue, outcome: now))
        }
        return steps
    }

    /// A stored outcome under iOS's answer: location's fine is coarse under reduced accuracy.
    private static func adjusted(_ stored: Int32, group: PermissionGroup, _ state: IOSState) -> Int32 {
        group == .location && state == .grantedReduced ? 2 : stored
    }

    /// The outcome of a grant of `ask` under iOS's answer: 1, for location 2 when coarse.
    private static func granted(_ ask: PermissionAsk, _ state: IOSState) -> Int32 {
        guard ask.group == .location else { return 1 }
        return ask.detail == 2 || state == .grantedReduced ? 2 : 1
    }

    private mutating func finish(_ state: IOSState, ios: IOSStates) -> [Step] {
        guard let shown else { return [] }
        self.shown = nil
        return [answer(shown, state.grants ? Self.granted(shown.ask, state) : 0)] + next(ios: ios)
    }

    /// The answer of a request, which the record keeps.
    private mutating func answer(_ request: Request, _ outcome: Int32) -> Step {
        let group = request.ask.group
        if outcome != 0 {
            records[group] = PermissionRecord(outcome: outcome, denials: 0)
        } else {
            records[group].outcome = 0
            records[group].denials += 1
        }
        return .answer(id: request.id, group: group.rawValue, outcome: outcome)
    }

    /// The next waiting request: answered at once from the record or iOS's denial, or shown.
    private mutating func next(ios: IOSStates) -> [Step] {
        var steps: [Step] = []
        while !waiting.isEmpty {
            let first = waiting.removeFirst()
            let group = first.ask.group, record = records[group], state = ios(group)
            // A stored grant answers, but a coarse one for a fine request asks again.
            let covers = record.outcome == 1 || (record.outcome == 2 && Self.granted(first.ask, state) == 2)
            if record.outcome != 0 && covers && state.grants {
                steps.append(.answer(id: first.id, group: group.rawValue,
                                     outcome: Self.adjusted(record.outcome, group: group, state)))
            } else if record.denials >= 2 {
                steps.append(.answer(id: first.id, group: group.rawValue, outcome: 0))
            } else if state == .denied {
                // iOS's no is the user's in Settings, not the app's: the record keeps no grant but counts no denial,
                // so that Minato's alert comes again once iOS allows the group.
                records[group].outcome = 0
                steps.append(.answer(id: first.id, group: group.rawValue, outcome: 0))
            } else {
                shown = first
                steps.append(.ask(id: first.id, group: group))
                break
            }
        }
        return steps
    }
}

extension PermissionDesk {
    /// The same iOS answer for every group.
    public mutating func requested(id: UInt64, text: String, ios: IOSState) -> [Step] {
        requested(id: id, text: text, ios: { _ in ios })
    }

    public mutating func allowed(id: UInt64, ios: IOSState) -> [Step] {
        allowed(id: id, ios: { _ in ios })
    }

    public mutating func iosAnswered(id: UInt64, granted: Bool) -> [Step] {
        iosAnswered(id: id, state: granted ? .granted : .denied)
    }

    public mutating func cancelled(id: UInt64, ios: IOSState) -> [Step] {
        cancelled(id: id, ios: { _ in ios })
    }

    public func seeds(ios: IOSState) -> [Step] {
        seeds(ios: { _ in ios })
    }

    public mutating func foreground(ios: IOSState) -> [Step] {
        foreground(ios: { _ in ios })
    }
}
