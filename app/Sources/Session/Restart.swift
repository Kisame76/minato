import APKSession
import UIKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

/// Ends Minato and starts it again (ADR-0032, decision 2): into game mode after the library wrote the
/// launch request, or back to the library without one. Each mode is a process of its own.
enum Restart {
    /// The ways of decision 2, tried in RestartSequence's order: the workspace, then our own URL.
    typealias Way = RestartWay

    /// The only way a Debug build tries when tools/session-check.sh, whose request the restarted process reads
    /// too, or the launch argument -restartWay asks for one; nil for the default order.
    static var requestedWay: Way? {
        #if DEBUG
        return SessionCheck.requestedWay ?? UserDefaults.standard.string(forKey: "restartWay").flatMap(Way.init)
        #else
        return nil
        #endif
    }

    /// How long each way's request may go unanswered before the next way, or the fallback after the last. The
    /// answer comes in well under a second; a way that stops answering on a later iOS (R10) still ends there.
    static let answerTimeout: Duration = .seconds(3)

    /// Whether this process restarts already. It restarts once, whatever asks again: a second tap, or a
    /// change of the scene while the fallback's alert waits.
    @MainActor private(set) static var isUnderway = false
    /// The ways tried so far; an answer or a timeout of a way it has left behind changes nothing.
    @MainActor private static var sequence = RestartSequence(requested: nil)

    /// Asks iOS to start Minato again and ends this process once a way's request is answered. When no way
    /// restarts, refused, unable to run or unanswered within `answerTimeout` each, `failed` runs instead, on
    /// the main actor: the caller shows the fallback, and the launch request stays for the next launch. The
    /// request waits until Minato's scene is active (whenActive).
    @MainActor
    static func now(through requested: Way? = requestedWay, failed: @escaping @MainActor @Sendable () -> Void) {
        guard !isUnderway else { return }
        isUnderway = true
        sequence = RestartSequence(requested: requested)
        whenActive { follow(sequence.start(), failed: failed) }
    }

    /// Runs `work` at once while Minato is active, else on the main queue's turn after it becomes active. While iOS
    /// still brings Minato to the front for a notification's tap, it answers a reopen of Minato "opened" but starts
    /// no new process then. Measured on the iPhone, from the Home Screen and from a launch by the tap: "restart
    /// workspace" at state inactive, answered, exit, and no process until the user opened Minato by hand. A tap
    /// in the library restarts while active and its new process starts 0.1 s later.
    @MainActor
    static func whenActive(_ work: @escaping @MainActor @Sendable () -> Void) {
        if UIApplication.shared.applicationState == .active { return work() }
        #if DEBUG
        SessionCheck.event("restart waits for active")
        #endif
        let token = Observation()
        token.value = NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { _ in
            MainActor.assumeIsolated {
                guard let observer = token.value else { return }
                token.value = nil
                NotificationCenter.default.removeObserver(observer)
                DispatchQueue.main.async { work() }
            }
        }
    }

    /// The observer of whenActive, which its own block removes.
    private final class Observation: @unchecked Sendable {
        var value: NSObjectProtocol?
    }

    @MainActor
    private static func follow(_ step: RestartSequence.Step?, failed: @escaping @MainActor @Sendable () -> Void) {
        switch step {
        case .attempt(let way):
            attempt(way, failed: failed)
        case .exit:
            #if DEBUG
            SessionCheck.event("exit")
            #endif
            // Without destructors: a guest's threads may still run or hold locks that they would meet
            // (apkrt.h, the end handler).
            _exit(0)
        case .fallback:
            #if DEBUG
            SessionCheck.restartFailed()
            #endif
            failed()
        case nil:
            break
        }
    }

    @MainActor
    private static func attempt(_ way: Way, failed: @escaping @MainActor @Sendable () -> Void) {
        log.info("Restarting through \(way.rawValue, privacy: .public)")
        #if DEBUG
        SessionCheck.event("restart \(way.rawValue)")
        SessionCheck.event("restart state \(SessionCheck.appState)")
        #endif
        let giveUp: @MainActor @Sendable (_ why: String) -> Void = { why in
            guard let next = Restart.sequence.failed(way) else { return }
            log.error("Restarting through \(way.rawValue, privacy: .public) failed: \(why, privacy: .public)")
            #if DEBUG
            SessionCheck.event("restartFailed \(way.rawValue)")
            #endif
            follow(next, failed: failed)
        }
        let answered: @Sendable (Bool) -> Void = { opened in
            Task { @MainActor in
                #if DEBUG
                if SessionCheck.dropsAnswer() { return }
                #endif
                if opened {
                    follow(Restart.sequence.answered(way), failed: failed)
                } else {
                    giveUp("the request was refused")
                }
            }
        }
        Task { @MainActor in
            try? await Task.sleep(for: answerTimeout)
            giveUp("no answer")
        }
        switch way {
        case .ownURL:
            guard let url = URL(string: "minato://open") else { return giveUp("no URL") }
            // The first request reaches this process, which ends once it is answered; the second starts
            // Minato again when the process is gone.
            UIApplication.shared.open(url) { answered($0) }
            UIApplication.shared.open(url)
        case .workspace:
            if !MinatoReopenThroughWorkspace(Bundle.main.bundleIdentifier ?? "io.github.kisame76.minato", answered) {
                giveUp("LSApplicationWorkspace is missing")
            }
        case .none:
            giveUp("no way")
        }
    }

    /// The URL of the restart itself, which the running app ignores.
    static func isRestart(_ url: URL) -> Bool {
        url.scheme == "minato" && url.host() == "open"
    }
}
