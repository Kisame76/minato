import APKSession
import Foundation
import Synchronization
import UserNotifications

/// ADR-0074: the guest's notifications on UNUserNotificationCenter. Here iOS's answer for them, which is Minato's for
/// every app: UNUserNotificationCenter gives it only through a callback, so it is kept, read again at each start and
/// return to the foreground, and waited for at most half a second when it is needed before the first read came.
final class NotificationHost: Sendable {
    static let shared = NotificationHost()

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

    private static func state(_ status: UNAuthorizationStatus) -> PermissionDesk.IOSState {
        switch status {
        case .notDetermined: .notDetermined
        case .denied: .denied
        default: .granted  // authorized, provisional, ephemeral
        }
    }
}
