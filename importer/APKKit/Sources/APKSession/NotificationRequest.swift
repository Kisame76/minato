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
    /// The content intent's Intent.toUri for a cold tap (ADR-0074 decision 4), empty for none.
    public var intent: String = ""
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

    /// The packages of the check's test apps, the only ones whose notifications a check line names.
    public static let checkPackages = "io.github.kisame76.minato.test."

    /// java-start-check's line for iOS's delivered list (ADR-0074 decision 3, the Debug host while the check's folder
    /// is there): "[<tag>|<id> <title>, ...]" of `package`'s own notifications, sorted; nil for an app that is not one
    /// of the check's test apps, so that no other app's notification, title or count gets into it.
    public static func checkLine(delivered: [(identifier: String, title: String)], package: String) -> String? {
        guard package.hasPrefix(checkPackages) else { return nil }
        let prefix = prefix(package: package)
        let mine = delivered.filter { $0.identifier.hasPrefix(prefix) }
            .map { "\($0.identifier.dropFirst(prefix.count)) \($0.title)" }.sorted()
        return "[\(mine.joined(separator: ", "))]"
    }

    /// java-start-check's line for a presentation in front: "<tag>|<id> banner list sound", the options in that
    /// order; nil for an app that is not one of the check's test apps, or an identifier not of `package`.
    public static func checkLine(presented identifier: String, options: Set<Presentation>, package: String) -> String? {
        let prefix = prefix(package: package)
        guard package.hasPrefix(checkPackages), identifier.hasPrefix(prefix) else { return nil }
        let names = [(Presentation.banner, "banner"), (.list, "list"), (.sound, "sound")]
            .filter { options.contains($0.0) }.map(\.1)
        return ([String(identifier.dropFirst(prefix.count))] + names).joined(separator: " ")
    }

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
/// \t<subText>\t<intent>\t<text>" (the text last, the only field with new lines), "cancel\t<id>\t<tag>" or
/// "cancelAll". The runtime checked it whole; nil for one the host cannot read all the same.
public enum NotificationRequest: Sendable, Equatable {
    case post(NotificationPost)
    case cancel(id: Int32, tag: String)
    case cancelAll

    public init?(text: String) {
        let fields = text.split(separator: "\t", maxSplits: 11, omittingEmptySubsequences: false).map(String.init)
        switch fields.first {
        case "post":
            guard fields.count == 12, let id = Int32(fields[1]), let importance = Int(fields[4]),
                  (0...5).contains(importance), fields[5] == "0" || fields[5] == "1", let when = Int64(fields[6])
            else { return nil }
            self = .post(NotificationPost(id: id, tag: fields[2], channel: fields[3], importance: importance,
                                          sound: fields[5] == "1", when: when, group: fields[7], title: fields[8],
                                          subText: fields[9], intent: fields[10], text: fields[11]))
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
