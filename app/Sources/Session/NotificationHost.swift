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
    /// notification's stored content intent, which the library checks before it starts with it. A tap that came
    /// before the library set it, as one that launched Minato can, waits for it.
    @MainActor static var onColdTap: ((_ package: String, _ intent: String?) -> Void)? {
        didSet {
            guard let onColdTap, let tap = waitingColdTap else { return }
            waitingColdTap = nil
            onColdTap(tap.package, tap.intent)
        }
    }

    /// A tap of no session that came before the library set onColdTap.
    @MainActor private static var waitingColdTap: (package: String, intent: String?)?

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

    // The delegate's methods take UIKit's completion handlers and call them on the main thread. Their async variants
    // return on Swift's cooperative pool, where UIKit asserts (the snapshot after didReceive's completion aborted
    // Minato at every tap on the iPhone).

    func userNotificationCenter(
        _ center: UNUserNotificationCenter, willPresent notification: UNNotification,
        withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void
    ) {
        let importance = notification.request.content.userInfo[Self.importanceKey] as? Int ?? 3
        var options: UNNotificationPresentationOptions = []
        for shown in NotificationPost.presentation(importance: importance) {
            switch shown {
            case .banner: options.insert(.banner)
            case .list: options.insert(.list)
            case .sound: options.insert(.sound)
            }
        }
        let done = Carried(value: { completionHandler(options) })
        Self.onMain { done.value() }
    }

    /// A tap on a notification (ADR-0074 decision 4): while the app's session runs, the guest sends the notification's
    /// content intent (APKRT_EVENT_NOTIFICATION_TAP); otherwise iOS has brought Minato to the front.
    func userNotificationCenter(
        _ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
        withCompletionHandler completionHandler: @escaping () -> Void
    ) {
        let key = response.actionIdentifier == UNNotificationDefaultActionIdentifier
            ? Self.key(response.notification.request.content.userInfo) : nil
        let done = Carried(value: completionHandler)
        Self.onMain {
            if let key { Self.tapped(key) }
            done.value()
        }
    }

    /// Runs `work` on the main thread: at once when this is it, else next on the main queue.
    private static func onMain(_ work: @escaping @MainActor @Sendable () -> Void) {
        if Thread.isMainThread {
            MainActor.assumeIsolated(work)
        } else {
            DispatchQueue.main.async(execute: work)
        }
    }

    /// A value UserNotifications hands over, such as a completion handler of the delegate's, carried to the main
    /// thread.
    private struct Carried<Value>: @unchecked Sendable {
        let value: Value
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
        SessionCheck.event("notification tap state \(SessionCheck.appState) session \(GameSession.current != nil)")
        #endif
        if let session = GameSession.current {
            session.notificationTapped(package: key.package, id: key.id, tag: key.tag)
        } else if let onColdTap {
            onColdTap(key.package, key.intent)
        } else {
            waitingColdTap = (key.package, key.intent)
        }
    }

    #if DEBUG
    /// session-check's notification-tap: a tap on the app's newest delivered notification. Its response goes to the
    /// center's delegate through the Objective-C runtime, as UIKit sends a tap's, with a completion handler that logs
    /// whether it ran on the main thread, where UIKit's must (a completion off it aborted Minato at every tap). The
    /// response is made by UNNotificationResponse's own factory, which the SDK does not declare: Debug builds only.
    func tapNewest(package: String) {
        let center = UNUserNotificationCenter.current()
        center.getDeliveredNotifications { delivered in
            let carried = Self.response(newestOf: package, in: delivered).map { Carried(value: $0) }
            DispatchQueue.main.async { Self.send(carried) }
        }
    }

    /// The same at once, on the main thread, as iOS calls didReceive while it brings Minato to the front: before the
    /// scene is active, at the launch a tap made or at the return from the background. It waits for the delivered
    /// notifications (at most 2 s).
    @MainActor func tapNewestNow(package: String) {
        let found = Mutex<Carried<UNNotificationResponse>?>(nil)
        let read = DispatchSemaphore(value: 0)
        UNUserNotificationCenter.current().getDeliveredNotifications { delivered in
            found.withLock { $0 = Self.response(newestOf: package, in: delivered).map { Carried(value: $0) } }
            read.signal()
        }
        _ = read.wait(timeout: .now() + 2)
        Self.send(found.withLock { $0 })
    }

    /// The response to the app's newest delivered notification, kept at `url` for a later process (secure coding), so
    /// that a later launch or return can hand it to the delegate at once, as fast as iOS's own didReceive comes.
    func saveNewestResponse(package: String, to url: URL) {
        UNUserNotificationCenter.current().getDeliveredNotifications { delivered in
            guard let response = Self.response(newestOf: package, in: delivered),
                  let data = try? NSKeyedArchiver.archivedData(withRootObject: response, requiringSecureCoding: true),
                  (try? data.write(to: url)) != nil
            else { return SessionCheck.event("notification response not saved") }
            SessionCheck.event("notification response saved")
        }
    }

    /// The response kept at `url` to the delegate at once, on the main thread; false when there is none.
    @MainActor func tapSaved(from url: URL) -> Bool {
        guard let data = try? Data(contentsOf: url),
              let response = try? NSKeyedUnarchiver.unarchivedObject(ofClass: UNNotificationResponse.self, from: data)
        else { return false }
        try? FileManager.default.removeItem(at: url)
        Self.send(Carried(value: response))
        return true
    }

    /// A response to the app's newest delivered notification, by UNNotificationResponse's own factory.
    private static func response(newestOf package: String, in delivered: [UNNotification]) -> UNNotificationResponse? {
        let mine = delivered.filter { ($0.request.content.userInfo[packageKey] as? String) == package }
        guard let newest = mine.max(by: { $0.date < $1.date }) else { return nil }
        let make = NSSelectorFromString("responseWithNotification:actionIdentifier:")
        let made = (UNNotificationResponse.self as AnyObject)
            .perform(make, with: newest, with: UNNotificationDefaultActionIdentifier)?.takeUnretainedValue()
        return made as? UNNotificationResponse
    }

    /// The response to the center's delegate through the Objective-C runtime, as UIKit sends a tap's.
    @MainActor private static func send(_ response: Carried<UNNotificationResponse>?) {
        guard let response else { return SessionCheck.event("notification tap: none delivered") }
        let center = UNUserNotificationCenter.current()
        center.delegate?.userNotificationCenter?(center, didReceive: response.value) {
            SessionCheck.event("notification response completed on the main thread \(Thread.isMainThread)")
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
