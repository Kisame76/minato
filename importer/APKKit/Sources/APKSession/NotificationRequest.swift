/// A posted notification of the guest's (ADR-0074 decisions 2 and 3), as APKRT_REQUEST_NOTIFICATION's "post" carries
/// it: NotificationManager.notify's id and tag, the channel and its importance (NotificationManager.IMPORTANCE_NONE 0
/// to MAX 5), whether the channel sounds, `when` in ms since the epoch (0 for none), the group, and the content's
/// title, sub text and text (bigText when the app gave one).
public struct NotificationPost: Sendable, Equatable {
    public var id: Int32
    public var tag: String
    public var channel: String
    public var importance: Int
    public var sound: Bool
    public var when: Int64
    public var group: String
    public var title: String
    public var subText: String
    public var text: String

    /// How a notification shows while the app is in front, where Android shows a heads-up for HIGH and above.
    public enum Presentation: Sendable, Hashable {
        case banner, list, sound
    }

    /// The request's identifier, "<package>|<tag>|<id>", so that a post again with the same tag and id replaces it,
    /// as on Android.
    public func identifier(package: String) -> String {
        Self.identifier(package: package, tag: tag, id: id)
    }

    public static func identifier(package: String, tag: String, id: Int32) -> String {
        "\(package)|\(tag)|\(id)"
    }

    /// What every identifier of the app starts with, for a cancel of all.
    public static func prefix(package: String) -> String {
        "\(package)|"
    }

    /// iOS's thread: the app's group, or else its channel.
    public func thread(package: String) -> String {
        "\(package)|\(group.isEmpty ? channel : group)"
    }

    /// IMPORTANCE_NONE (0) shows nothing, as Android blocks the channel.
    public var shows: Bool { importance > 0 }
    /// LOW (2) and MIN (1) are quiet: iOS's passive level.
    public var passive: Bool { importance <= 2 }
    /// DEFAULT (3) and above sound, when the channel has a sound.
    public var plays: Bool { sound && importance >= 3 }

    public static func presentation(importance: Int) -> Set<Presentation> {
        switch importance {
        case 4...: [.banner, .list, .sound]
        case 3: [.list, .sound]
        case 1...2: [.list]
        default: []
        }
    }
}

/// APKRT_REQUEST_NOTIFICATION's text: "post\t<id>\t<tag>\t<channel>\t<importance>\t<sound>\t<when>\t<group>\t<title>
/// \t<subText>\t<text>" (the text last, the only field with new lines), "cancel\t<id>\t<tag>" or "cancelAll". The
/// runtime checked it whole; nil for one the host cannot read all the same.
public enum NotificationRequest: Sendable, Equatable {
    case post(NotificationPost)
    case cancel(id: Int32, tag: String)
    case cancelAll

    public init?(text: String) {
        let fields = text.split(separator: "\t", maxSplits: 10, omittingEmptySubsequences: false).map(String.init)
        switch fields.first {
        case "post":
            guard fields.count == 11, let id = Int32(fields[1]), let importance = Int(fields[4]),
                  (0...5).contains(importance), fields[5] == "0" || fields[5] == "1", let when = Int64(fields[6])
            else { return nil }
            self = .post(NotificationPost(id: id, tag: fields[2], channel: fields[3], importance: importance,
                                          sound: fields[5] == "1", when: when, group: fields[7], title: fields[8],
                                          subText: fields[9], text: fields[10]))
        case "cancel":
            guard fields.count == 3, let id = Int32(fields[1]) else { return nil }
            self = .cancel(id: id, tag: fields[2])
        case "cancelAll":
            guard fields.count == 1 else { return nil }
            self = .cancelAll
        default:
            return nil
        }
    }
}
