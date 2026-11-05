/// tools/session-window-check.sh's stand-in for NotificationHost.swift, whose file needs the session and the library:
/// the view's app delegate starts it at the launch (ADR-0074 decision 4), which the check does not reach.
final class NotificationHost: Sendable {
    static let shared = NotificationHost()

    func start() {}
}
