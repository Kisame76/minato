import UIKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

/// Ends Minato and starts it again (ADR-0032, decision 2): into game mode after the library wrote the
/// launch request, or back to the library without one. Each mode is a process of its own.
enum Restart {
    /// The ways of decision 2; the first that works in the Simulator and on the iPhone is the default.
    enum Way: String, CaseIterable, Sendable {
        /// minato://open, opened twice with UIApplication.open (public API).
        case ownURL = "url"
        /// LSApplicationWorkspace (private API, RestartWorkspace.m).
        case workspace
        /// Neither: the alert asks the user to open Minato again.
        case none
    }

    /// The way this build restarts. A Debug build takes another from tools/session-check.sh, whose request
    /// the restarted process reads too, or from the launch argument -restartWay.
    static var way: Way {
        #if DEBUG
        if let way = SessionCheck.requestedWay ?? UserDefaults.standard.string(forKey: "restartWay").flatMap(Way.init) {
            return way
        }
        #endif
        return .ownURL
    }

    /// How long the first request may go unanswered before the fallback shows. The answer comes in well
    /// under a second; a way that stops answering on a later iOS (R10) still ends in the fallback.
    static let answerTimeout: Duration = .seconds(3)

    /// Whether this process restarts already. It restarts once, whatever asks again: a second tap, or a
    /// change of the scene while the fallback's alert waits.
    @MainActor private(set) static var isUnderway = false
    /// The first answer or the timeout decided; what comes later changes nothing.
    @MainActor private static var isSettled = false

    /// Asks iOS to start Minato again and ends this process once the first request is answered. When the
    /// way cannot restart, or no answer comes within `answerTimeout`, `failed` runs instead, on the main
    /// actor: the caller shows the fallback, and the launch request stays for the next launch.
    @MainActor
    static func now(through way: Way = way, failed: @escaping @MainActor @Sendable () -> Void) {
        guard !isUnderway else { return }
        isUnderway = true
        log.info("Restarting through \(way.rawValue, privacy: .public)")
        #if DEBUG
        SessionCheck.event("restart \(way.rawValue)")
        #endif
        let settle: @MainActor @Sendable (_ opened: Bool, _ why: String) -> Void = { opened, why in
            guard !Restart.isSettled else { return }
            Restart.isSettled = true
            if opened {
                #if DEBUG
                SessionCheck.event("exit")
                #endif
                exit(0)
            }
            log.error("Restarting through \(way.rawValue, privacy: .public) failed: \(why, privacy: .public)")
            #if DEBUG
            SessionCheck.event("restartFailed \(way.rawValue)")
            SessionCheck.restartFailed()
            #endif
            failed()
        }
        let answered: @Sendable (Bool) -> Void = { opened in
            Task { @MainActor in
                #if DEBUG
                if SessionCheck.dropsAnswer() { return }
                #endif
                settle(opened, "the request was refused")
            }
        }
        Task { @MainActor in
            try? await Task.sleep(for: answerTimeout)
            settle(false, "no answer")
        }
        switch way {
        case .ownURL:
            guard let url = URL(string: "minato://open") else { return settle(false, "no URL") }
            // The first request reaches this process, which ends once it is answered; the second starts
            // Minato again when the process is gone.
            UIApplication.shared.open(url) { answered($0) }
            UIApplication.shared.open(url)
        case .workspace:
            if !MinatoReopenThroughWorkspace(Bundle.main.bundleIdentifier ?? "io.github.kisame76.minato", answered) {
                settle(false, "LSApplicationWorkspace is missing")
            }
        case .none:
            settle(false, "no way")
        }
    }

    /// The URL of the restart itself, which the running app ignores.
    static func isRestart(_ url: URL) -> Bool {
        url.scheme == "minato" && url.host() == "open"
    }
}
