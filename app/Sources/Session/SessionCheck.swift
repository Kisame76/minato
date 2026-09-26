#if DEBUG
import APKLibrary
import APKSession
import Darwin
import Foundation
import UIKit

/// Answers tools/session-check.sh in the Simulator and on the iPhone (ADR-0032, test 6). A restarted process
/// gets no launch arguments, so the script leaves its requests as files in tmp/session-check/, which every process
/// reads; nothing happens without that folder. Each process appends what it does to
/// tmp/session-check/events, one line per event: "<seconds since 1970> <pid> <event>".
///   way      "url", "workspace" or "none": how both modes restart (Restart.way).
///   tap      a package: the library taps that app once its entries are loaded, and removes the file.
///   quit     game mode opens its menu 1.5 s after its start and chooses Quit 1.5 s later.
///   confirm  the fallback's alert, once it is up, answers OK by itself after the seconds the file names
///            (1 when it names none), which ends the process.
///   orientation  "landscape" or "portrait": game mode holds that instead of the app's own.
///   unanswered   the next restart's first request goes unanswered, so it times out; the file is removed.
///   forget-hint  the next process forgets that game mode showed its hint, and removes the file.
///   end      a package: the library removes that app, then the whole folder, which ends the check. On the
///            iPhone the script cannot delete files in the app's container itself.
/// A request's file says "yes" when it names nothing; an empty file is none.
enum SessionCheck {
    static let folder = URL.temporaryDirectory.appending(path: "session-check", directoryHint: .isDirectory)

    private static var isOn: Bool { FileManager.default.fileExists(atPath: folder.path) }

    /// An empty file is no request: on the iPhone the script cannot delete one, only empty it.
    private static func requested(_ name: String) -> Bool {
        text(of: name) != nil
    }

    private static func text(of name: String) -> String? {
        guard isOn, let text = try? String(contentsOf: folder.appending(path: name), encoding: .utf8) else { return nil }
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }

    /// The way the script asks for.
    static var requestedWay: Restart.Way? { text(of: "way").flatMap(Restart.Way.init) }

    /// The orientation the script asks game mode to hold.
    static var requestedOrientation: SessionOrientation? {
        switch text(of: "orientation") {
        case "landscape": .landscape
        case "portrait": .portrait
        default: nil
        }
    }

    /// Whether the restart's first request goes unanswered, once.
    @MainActor
    static func dropsAnswer() -> Bool {
        guard requested("unanswered") else { return false }
        try? FileManager.default.removeItem(at: folder.appending(path: "unanswered"))
        event("answer dropped")
        return true
    }

    static func event(_ text: String) {
        guard isOn else { return }
        let line = String(format: "%.3f", Date.now.timeIntervalSince1970) + " \(getpid()) \(text)\n"
        let file = folder.appending(path: "events")
        if let handle = try? FileHandle(forWritingTo: file) {
            defer { try? handle.close() }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: Data(line.utf8))
        } else {
            try? Data(line.utf8).write(to: file)
        }
    }

    /// The mode chosen at launch, and the seconds since the launch request was written.
    static func processStarted(_ mode: LaunchMode, request: LaunchRequest?) {
        if requested("forget-hint") {
            UserDefaults.standard.removeObject(forKey: GameSession.hintShownKey)
            try? FileManager.default.removeItem(at: folder.appending(path: "forget-hint"))
        }
        let seconds = request.map { String(format: " %.3f", Date.now.timeIntervalSince($0.time)) } ?? ""
        switch mode {
        case .gameMode(let entry): event("process gameMode \(entry.id)\(seconds)")
        case .library(nil): event("process library\(seconds)")
        case .library(.appNotFound(let package)): event("process library appNotFound \(package)\(seconds)")
        case .library(.appNotReady(let entry)): event("process library appNotReady \(entry.id)\(seconds)")
        }
    }

    /// The library taps the requested app once, as the user would.
    @MainActor
    static func tapIfRequested(in library: LibraryModel) async {
        let file = folder.appending(path: "tap")
        guard let package = text(of: "tap"), let entry = library.entries.first(where: { $0.id == package }) else { return }
        try? FileManager.default.removeItem(at: file)
        event("tap \(package)")
        switch await library.launch(entry) {
        case nil: break
        case .openSigning: event("blocked openSigning")
        case .explain(_, let reason): event("blocked \(reason)")
        }
    }

    /// The check is over: the library removes the requested app, then the folder.
    @MainActor
    static func endIfRequested(in library: LibraryModel) async {
        guard let package = text(of: "end") else { return }
        if let entry = library.entries.first(where: { $0.id == package }) {
            await library.remove([entry])
        }
        try? FileManager.default.removeItem(at: folder)
    }

    /// Game mode's guest starts: the window's size in pixels it starts with, the window's facts, then the
    /// menu and Quit when the script asks for them.
    @MainActor
    static func gameModeStarted(_ session: GameSession) {
        guard isOn else { return }
        event("start window \(Int(session.windowPixels.width))x\(Int(session.windowPixels.height))")
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(0.5))
            recordWindow()
            guard requested("quit") else { return }
            try? await Task.sleep(for: .seconds(1))
            session.openMenu()
            try? await Task.sleep(for: .seconds(1.5))
            session.quit()
        }
    }

    /// The restart failed: whether the fallback's alert comes up, with its title, and with the request
    /// "confirm" its OK.
    @MainActor
    static func restartFailed() {
        guard isOn else { return }
        Task { @MainActor in
            var title: String?
            for _ in 0..<50 where title == nil {
                try? await Task.sleep(for: .seconds(0.1))
                title = alertTitle()
            }
            event(title.map { "fallback shown \($0)" } ?? "fallback not shown, on top: \(topController())")
            guard requested("confirm") else { return }
            try? await Task.sleep(for: .seconds(text(of: "confirm").flatMap(Double.init) ?? 1))
            event("fallback confirmed")
            exit(0)
        }
    }

    /// The title of the alert on top of the window, if one is up.
    @MainActor
    private static func alertTitle() -> String? {
        guard let alert = topmost() as? UIAlertController, alert.preferredStyle == .alert else { return nil }
        return alert.title
    }

    @MainActor
    private static func topController() -> String {
        topmost().map { String(describing: type(of: $0)) } ?? "nothing"
    }

    @MainActor
    private static func topmost() -> UIViewController? {
        guard let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene,
              let window = scene.windows.first(where: \.isKeyWindow) ?? scene.windows.first
        else { return nil }
        var controller = window.rootViewController
        while let presented = controller?.presentedViewController { controller = presented }
        return controller
    }

    /// The layer's size in pixels and whether it covers the window, the screen's, the orientation, and
    /// what the window asks of the system: status bar, home indicator, and the edges that defer gestures.
    @MainActor
    private static func recordWindow() {
        guard let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene,
              let window = scene.windows.first(where: \.isKeyWindow) ?? scene.windows.first,
              let root = window.rootViewController
        else { return event("window none") }
        let host = metalLayerView(in: window)
        let pixels = host.map { "\(Int($0.bounds.width * $0.layer.contentsScale))x\(Int($0.bounds.height * $0.layer.contentsScale))" } ?? "none"
        let covers = host.map { $0.convert($0.bounds, to: window) == window.bounds } ?? false
        let screen = scene.screen
        let edges = deciding(root) { $0.childForScreenEdgesDeferringSystemGestures }.preferredScreenEdgesDeferringSystemGestures
        let homeIndicator = deciding(root) { $0.childForHomeIndicatorAutoHidden }.prefersHomeIndicatorAutoHidden
        event("window layer \(pixels) covers \(covers) screen \(Int(screen.nativeBounds.width))x\(Int(screen.nativeBounds.height)) "
            + "scale \(host?.layer.contentsScale ?? 0)/\(screen.nativeScale) orientation \(scene.effectiveGeometry.interfaceOrientation.rawValue) "
            + "statusBarHidden \(scene.statusBarManager?.isStatusBarHidden ?? false) homeIndicatorHidden \(homeIndicator) "
            + "deferredEdges \(edges.rawValue)")
    }

    /// The view controller UIKit asks, following the children a controller hands the question to.
    @MainActor
    private static func deciding(_ controller: UIViewController, child: (UIViewController) -> UIViewController?) -> UIViewController {
        var current = controller
        while let next = child(current), next !== current { current = next }
        return current
    }

    @MainActor
    private static func metalLayerView(in view: UIView) -> UIView? {
        if view.layer is CAMetalLayer { return view }
        for subview in view.subviews {
            if let found = metalLayerView(in: subview) { return found }
        }
        return nil
    }
}
#endif
