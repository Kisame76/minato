import APKSession
import Foundation
import Synchronization
import UserNotifications
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "notifications")

/// ADR-0074: the guest's notifications on UNUserNotificationCenter. iOS's answer for them, which is Minato's for
/// every app: UNUserNotificationCenter gives it only through a callback, so it is kept, read again at each start and
/// return to the foreground, and waited for at most half a second when it is needed before the first read came. The
/// app's posts and cancels (decision 3): a post replaces the delivered one of the same tag and id; while the app is in
/// front, its importance chooses the banner, the list and the sound, as Android's heads-up comes from HIGH on.
final class NotificationHost: NSObject, Sendable, UNUserNotificationCenterDelegate {
    static let shared = NotificationHost()
    /// The importance a request carries for willPresent, and the app, id and tag a tap names.
    private static let importanceKey = "minato.importance"
    private static let packageKey = "minato.package"
    private static let idKey = "minato.id"
    private static let tagKey = "minato.tag"
    private static let intentKey = "minato.intent"
    /// The library's start of an app for a tap while no session runs (ADR-0074 decision 4): the package and the
    /// notification's stored content intent, which the library checks before it starts with it.
    @MainActor static var onColdTap: ((_ package: String, _ intent: String?) -> Void)?

    private let known = Mutex<PermissionDesk.IOSState?>(nil)

    /// Reads iOS's answer again; `done` runs once it is kept, on a queue of UserNotifications'.
    func refresh(_ done: (@Sendable () -> Void)? = nil) {
        UNUserNotificationCenter.current().getNotificationSettings { [self] settings in
            known.withLock { $0 = Self.state(settings.authorizationStatus) }
            done?()
        }
    }

    /// iOS's answer as last read.
    var state: PermissionDesk.IOSState {
        if let state = known.withLock({ $0 }) { return state }
        let read = DispatchSemaphore(value: 0)
        refresh { read.signal() }
        _ = read.wait(timeout: .now() + 0.5)
        return known.withLock { $0 } ?? .notDetermined
    }

    /// iOS's prompt for alerts, sounds and badges, once Minato's alert said yes (ADR-0074 decision 1).
    func requestAuthorization(_ done: @escaping @MainActor @Sendable (PermissionDesk.IOSState) -> Void) {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge]) { [self] _, _ in
            refresh { [self] in
                let state = state
                Task { @MainActor in done(state) }
            }
        }
    }

    /// The session's start: Minato's delegate takes the notifications that come while it is in front.
    func start() {
        UNUserNotificationCenter.current().delegate = self
    }

    /// The guest's request, for the app `package` named `appName`, on the main thread.
    func handle(_ request: NotificationRequest, package: String, appName: String) {
        let center = UNUserNotificationCenter.current()
        switch request {
        case .post(let note):
            let identifier = note.identifier(package: package)
            guard note.shows else { return center.removeDeliveredNotifications(withIdentifiers: [identifier]) }
            let content = UNMutableNotificationContent()
            content.title = note.title
            content.subtitle = note.subText.isEmpty ? appName : "\(appName) · \(note.subText)"
            content.body = note.text
            content.threadIdentifier = note.thread(package: package)
            content.interruptionLevel = note.passive ? .passive : .active
            content.sound = note.plays ? .default : nil
            content.userInfo = [Self.importanceKey: note.importance, Self.packageKey: package, Self.idKey: note.id,
                                Self.tagKey: note.tag, Self.intentKey: note.intent]
            center.add(UNNotificationRequest(identifier: identifier, content: content, trigger: nil)) { error in
                guard let error else { return }
                log.error("notification \(identifier, privacy: .public) failed: \(error.localizedDescription)")
            }
            #if DEBUG
            SessionCheck.event("notification posted \(note.tag)|\(note.id) importance \(note.importance)")
            #endif
        case .cancel(let id, let tag):
            let identifier = NotificationPost.identifier(package: package, tag: tag, id: id)
            center.removeDeliveredNotifications(withIdentifiers: [identifier])
            #if DEBUG
            SessionCheck.event("notification cancelled \(tag)|\(id)")
            #endif
        case .cancelAll:
            removeAll(package: package)
        }
    }

    /// Every delivered notification of the app goes: its cancelAll, or its removal from the library.
    func removeAll(package: String) {
        let center = UNUserNotificationCenter.current()
        let prefix = NotificationPost.prefix(package: package)
        center.getDeliveredNotifications { delivered in
            let mine = delivered.map(\.request.identifier).filter { $0.hasPrefix(prefix) }
            if !mine.isEmpty { center.removeDeliveredNotifications(withIdentifiers: mine) }
        }
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification)
        async -> UNNotificationPresentationOptions {
        let importance = notification.request.content.userInfo[Self.importanceKey] as? Int ?? 3
        var options: UNNotificationPresentationOptions = []
        for shown in NotificationPost.presentation(importance: importance) {
            switch shown {
            case .banner: options.insert(.banner)
            case .list: options.insert(.list)
            case .sound: options.insert(.sound)
            }
        }
        return options
    }

    /// A tap on a notification (ADR-0074 decision 4): while the app's session runs, the guest sends the notification's
    /// content intent (APKRT_EVENT_NOTIFICATION_TAP); otherwise iOS has brought Minato to the front.
    func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse) async {
        guard response.actionIdentifier == UNNotificationDefaultActionIdentifier else { return }
        guard let key = Self.key(response.notification.request.content.userInfo) else { return }
        await MainActor.run { Self.tapped(key) }
    }

    /// The app, id and tag a notification's userInfo names.
    private static func key(_ info: [AnyHashable: Any]) -> (package: String, id: Int32, tag: String, intent: String?)? {
        guard let package = info[packageKey] as? String, let number = info[idKey] as? NSNumber else { return nil }
        let intent = (info[intentKey] as? String).flatMap { $0.isEmpty ? nil : $0 }
        return (package, number.int32Value, info[tagKey] as? String ?? "", intent)
    }

    /// The tap on the notification `key` names, on the main thread: the running session's app sends its content intent;
    /// with no session the library starts the app (a cold tap); during another app's session it only brings Minato up.
    @MainActor private static func tapped(_ key: (package: String, id: Int32, tag: String, intent: String?)) {
        #if DEBUG
        SessionCheck.event("notification tapped \(key.tag)|\(key.id)")
        #endif
        if let session = GameSession.current {
            session.notificationTapped(package: key.package, id: key.id, tag: key.tag)
        } else {
            onColdTap?(key.package, key.intent)
        }
    }

    #if DEBUG
    /// session-check's notification-tap: a tap on the app's newest delivered notification, as didReceive takes it.
    func tapNewest(package: String) {
        UNUserNotificationCenter.current().getDeliveredNotifications { delivered in
            let mine = delivered.filter { ($0.request.content.userInfo[Self.packageKey] as? String) == package }
            guard let newest = mine.max(by: { $0.date < $1.date }) else {
                return SessionCheck.event("notification tap: none delivered")
            }
            guard let key = Self.key(newest.request.content.userInfo) else { return }
            Task { @MainActor in Self.tapped(key) }
        }
    }
    #endif

    private static func state(_ status: UNAuthorizationStatus) -> PermissionDesk.IOSState {
        switch status {
        case .notDetermined: .notDetermined
        case .denied: .denied
        default: .granted  // authorized, provisional, ephemeral
        }
    }
}
