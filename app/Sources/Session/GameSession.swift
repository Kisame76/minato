import APKLibrary
import APKSession
import Foundation
import Observation
import SwiftUI
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

/// One app's session in game mode (ADR-0032): its lifecycle, the menu, and the way back to the library.
/// The guest itself starts with ADR-0028's activity-host in step c); until then the callbacks are
/// logged and every wait for an answer ends at once.
@MainActor
@Observable
final class GameSession {
    let entry: LibraryEntry
    private(set) var menuIsOpen = false
    private(set) var showsHint: Bool
    /// The restart back to the library failed: the user opens Minato again.
    var restartFailed = false

    /// The window's size in pixels at its last layout.
    @ObservationIgnored private(set) var windowPixels = CGSize.zero
    /// The orientation iOS was asked to hold the window in.
    @ObservationIgnored private var heldOrientation: SessionOrientation?

    @ObservationIgnored private var lifecycle: Lifecycle
    @ObservationIgnored private var started = false

    nonisolated static let hintShownKey = "sessionMenuHintShown"

    init(entry: LibraryEntry) {
        self.entry = entry
        // Game mode starts only ready apps, which have a report; activity-host reports API level 35.
        lifecycle = Lifecycle(targetSdk: entry.report?.targetSdk ?? 35)
        showsHint = !UserDefaults.standard.bool(forKey: Self.hintShownKey)
    }

    /// The orientation the session is to hold (ADR-0032, decision 5).
    var orientation: SessionOrientation {
        #if DEBUG
        if let requested = SessionCheck.requestedOrientation { return requested }
        #endif
        return SessionOrientation(screenOrientation: entry.report?.screenOrientation)
    }

    /// iOS was asked to hold the window in `orientation`, which turns it after a while.
    func holds(_ orientation: SessionOrientation) {
        heldOrientation = orientation
        startIfLaidOut()
        // iOS turns the window within a fraction of a second; should it never do so, nor refuse, the
        // guest still starts, with the size the window has.
        Task {
            try? await Task.sleep(for: .seconds(2))
            guard !started else { return }
            heldOrientation = .atStart
            startIfLaidOut()
        }
    }

    /// The window was laid out.
    func windowLaidOut(_ pixels: CGSize) {
        windowPixels = pixels
        startIfLaidOut()
    }

    /// The guest starts with its window's final size, which ADR-0028 hands over with the window: at the
    /// first layout in the orientation the session holds.
    private func startIfLaidOut() {
        guard let heldOrientation, heldOrientation.fits(width: windowPixels.width, height: windowPixels.height) else { return }
        start()
    }

    /// The window is there; the guest starts with it, or with the foreground (Lifecycle).
    private func start() {
        guard !started else { return }
        started = true
        deliver(.start)
    }

    /// The guest started: the first session shows how to reach the menu for 3 s.
    private func guestStarted() {
        #if DEBUG
        SessionCheck.event(showsHint ? "hint shown" : "hint not shown")
        SessionCheck.gameModeStarted(self)
        #endif
        guard showsHint else { return }
        UserDefaults.standard.set(true, forKey: Self.hintShownKey)
        Task {
            try? await Task.sleep(for: .seconds(3))
            showsHint = false
        }
    }

    /// iOS moved the scene from `old` to `new`.
    func sceneChanged(from old: ScenePhase, to new: ScenePhase) {
        switch new {
        case .active:
            if old == .background { deliver(.foreground) }
            deliver(.active)
        case .inactive:
            deliver(old == .background ? .foreground : .inactive)
        case .background:
            if old == .active { deliver(.inactive) }
            deliver(.background)
        @unknown default:
            break
        }
    }

    func memoryWarning() {
        deliver(.memoryWarning)
    }

    func openMenu() {
        guard !menuIsOpen else { return }
        menuIsOpen = true
        #if DEBUG
        SessionCheck.event("menu opened")
        #endif
        deliver(.menuOpened)
    }

    func closeMenu() {
        guard menuIsOpen else { return }
        menuIsOpen = false
        #if DEBUG
        SessionCheck.event("menu closed")
        #endif
        deliver(.menuClosed)
    }

    /// Quit in the menu: the guest ends, then Minato restarts into the library.
    func quit() {
        menuIsOpen = false
        #if DEBUG
        SessionCheck.event("quit")
        #endif
        deliver(.quit)
    }

    private func deliver(_ event: Lifecycle.Event) {
        var callbacks = lifecycle.handle(event)
        let starts = callbacks.contains(.onCreate)
        // Nothing answers before the guest runs (step c).
        while let awaited = lifecycle.awaits {
            callbacks += lifecycle.handle(.answered(awaited))
        }
        for callback in callbacks {
            log.info("\(self.entry.id, privacy: .public): \(String(describing: callback), privacy: .public)")
            #if DEBUG
            SessionCheck.event("callback \(callback)")
            #endif
        }
        if starts { guestStarted() }
        if lifecycle.hasEnded {
            Restart.now { [weak self] in self?.restartFailed = true }
        }
    }
}
