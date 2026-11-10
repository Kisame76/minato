import APKLibrary
import APKSession
import Foundation
import UIKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "install")

/// An app installer over game mode (APKRT_REQUEST_ASK_INSTALL_SOURCES, APKRT_REQUEST_CONFIRM_INSTALL), one request at
/// a time, each answered once. The question whether the app may install apps is Minato's alert, its grant kept in the
/// app's profile (AppProfile.installSources), which the next start hands the runtime. A handed-over APK is taken only
/// from the app's own data folder (InstallFile), checked as the import would check it (Library.checkForInstall) and,
/// after the user's yes, taken into the library's incoming folder, which the library installs at its next start, as
/// "Open with" during a session does (ADR-0018); a session's set of APKs (base.apk and split_*.apk) likewise, as one
/// unit, which the library installs as one app (ADR-0048). The answers: granted or denied; ready, failed or cancelled.
/// An app's request to remove an app (APKRT_REQUEST_UNINSTALL, ADR-0077) names the target by its package: an app of the
/// library goes only after the user's yes in Minato's alert, by the library's own Delete (LibraryModel.remove); one
/// that is not there fails at once. The asking app is this session's, never the request's text; when it removed itself,
/// its session ends after the answer.
@MainActor
final class InstallPresenter {
    enum Request {
        case askSources(id: UInt64, text: String)
        case confirm(id: UInt64, text: String)
        case uninstall(id: UInt64, text: String)
    }

    private let package: String
    private let appName: String
    private let dataFolder: URL?
    private let paths: LibraryPaths?
    private let sourcesAnswered: (UInt64, Bool) -> Void
    private let installAnswered: (UInt64, InstallOutcome) -> Void
    private let uninstallAnswered: (UInt64, UninstallOutcome) -> Void
    private let removedItself: () -> Void
    private var waiting: [Request] = []
    private var busy = false

    init(package: String, appName: String, dataFolder: URL?, paths: LibraryPaths?,
         sourcesAnswered: @escaping (UInt64, Bool) -> Void,
         installAnswered: @escaping (UInt64, InstallOutcome) -> Void,
         uninstallAnswered: @escaping (UInt64, UninstallOutcome) -> Void, removedItself: @escaping () -> Void) {
        self.package = package
        self.appName = appName
        self.dataFolder = dataFolder
        self.paths = paths
        self.sourcesAnswered = sourcesAnswered
        self.installAnswered = installAnswered
        self.uninstallAnswered = uninstallAnswered
        self.removedItself = removedItself
    }

    func requested(_ request: Request) {
        waiting.append(request)
        next()
    }

    private func next() {
        guard !busy, !waiting.isEmpty else { return }
        busy = true
        switch waiting.removeFirst() {
        case .askSources(let id, let text): askSources(id, text: text)
        case .confirm(let id, let text): confirm(id, text: text)
        case .uninstall(let id, let text): uninstall(id, text: text)
        }
    }

    private func done() {
        busy = false
        next()
    }

    // MARK: - Whether the app may install apps

    private func askSources(_ id: UInt64, text: String) {
        // "<package>\n<label>", InstallFile's shape; the label the guest wrote is not shown, the alert names the app as
        // the library does.
        guard let paths, InstallFile.parse(text)?.path == package else { return answerSources(id, false) }
        if AppProfile.load(for: package, in: paths).installSources == true { return answerSources(id, true) }
        let app = AlertName.plain(appName).map { "“\($0)”" } ?? "this app"
        alert(title: "Allow \(app) to install apps?", message: "Apps it installs are added to your library.",
              no: "Don’t Allow", yes: "Allow", event: "install sources alert") { [weak self] allowed in
            guard let self else { return }
            if allowed {
                var profile = AppProfile.load(for: package, in: paths)
                profile.installSources = true
                do {
                    try profile.save(for: package, in: paths)
                } catch {
                    log.error("the install grant was not saved: \(error.localizedDescription, privacy: .public)")
                }
            }
            answerSources(id, allowed)
        }
    }

    private func answerSources(_ id: UInt64, _ granted: Bool) {
        log.info("install sources \(id): \(granted ? "granted" : "denied", privacy: .public)")
        #if DEBUG
        SessionCheck.event("install sources \(id) \(granted ? "granted" : "denied")")
        #endif
        sourcesAnswered(id, granted)
        done()
    }

    // MARK: - A handed-over APK

    private func confirm(_ id: UInt64, text: String) {
        guard let request = InstallFile.parse(text), request.package == package, let dataFolder, let paths,
              let guestPaths = InstallFile.paths(of: request.path) else {
            return refuse(id, "the request does not name a file of the app's own data")
        }
        let sets = guestPaths.compactMap { InstallFile.components(of: $0, package: package) }
        // Each file under its own name in one folder, which the library takes as APKs by their extension; two of the
        // same name are no set Android takes either.
        let names = sets.map { $0.last!.lowercased().hasSuffix(".apk") ? $0.last! : $0.last! + ".apk" }
        guard sets.count == guestPaths.count, Set(names).count == names.count else {
            return refuse(id, "the request does not name files of the app's own data")
        }
        // Without the user's grant Android's installer blocks the app and the user goes back: RESULT_CANCELED, as the
        // framework's own check answers.
        guard AppProfile.load(for: package, in: paths).installSources == true else {
            log.error("install \(id): the app may not install apps")
            return finish(id, .cancelled, removing: nil)
        }
        let copy = FileManager.default.temporaryDirectory.appending(path: "\(Self.copyPrefix)\(UUID().uuidString)",
                                                                    directoryHint: .isDirectory)
        // The room the copy may take: what the volume has free for it less a reserve; a larger APK fails as on an
        // Android device without the room (INSTALL_FAILED_INSUFFICIENT_STORAGE).
        let free = (try? FileManager.default.temporaryDirectory
            .resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
            .volumeAvailableCapacityForImportantUsage) ?? 0
        let limit = max(0, free - Self.reserve)
        Task {
            let check: Result<InstallCheck, Error> = await Task.detached {
                do {
                    try FileManager.default.createDirectory(at: copy, withIntermediateDirectories: true)
                    var room = limit
                    for (components, name) in zip(sets, names) {
                        let file = copy.appending(path: name)
                        try InstallFile.copy(components, below: dataFolder, to: file, limit: room)
                        let size = try? FileManager.default.attributesOfItem(atPath: file.path)[.size] as? Int
                        room -= Int64(size ?? 0)
                    }
                    // The import's own checks, as the library uses them (LibraryModel.useChecks).
                    let library = try Library(paths: paths)
                    await LibraryModel.useChecks(in: library, guest: Bundle.main.bundleURL.appending(path: "Guest"))
                    return .success(await library.checkForInstall(names.map { copy.appending(path: $0) }))
                } catch {
                    return .failure(error)
                }
            }.value
            switch check {
            case .failure(let error):
                refuse(id, "the file was not taken: \(error)", removing: copy)
            case .success(.refused(let why)):
                refuse(id, why, removing: copy)
            case .success(.acceptable(_, let label)):
                let name = AlertName.plain(label).map { "“\($0)”" } ?? "this app"
                let from = AlertName.plain(appName).map { "“\($0)”" } ?? "This app"
                alert(title: "Install \(name)?", message: "\(from) wants to add it to your library.",
                      no: "Cancel", yes: "Install", event: "install alert \(label)") { [weak self] install in
                    guard let self else { return }
                    guard install else { return finish(id, .cancelled, removing: copy) }
                    Task {
                        do {
                            _ = try await Library(paths: paths).adopt(names.map { copy.appending(path: $0) })
                            finish(id, .ready, removing: copy)
                        } catch {
                            refuse(id, "the library did not take it: \(error)", removing: copy)
                        }
                    }
                }
            }
        }
    }

    /// The copies' names in the temporary folder, and the room a copy leaves free.
    nonisolated private static let copyPrefix = "install-"
    private static let reserve: Int64 = 512 << 20

    /// The copies a game mode that ended halfway left behind, and the install checks' folders; at each launch.
    nonisolated static func removeLeftovers() {
        let folder = FileManager.default.temporaryDirectory
        for name in (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        where name.hasPrefix(copyPrefix) {
            try? FileManager.default.removeItem(at: folder.appending(path: name))
        }
    }

    private func refuse(_ id: UInt64, _ why: String, removing copy: URL? = nil) {
        log.error("install \(id) failed: \(why, privacy: .public)")
        #if DEBUG
        SessionCheck.event("install \(id) refused: \(why)")
        #endif
        finish(id, .failed, removing: copy)
    }

    private func finish(_ id: UInt64, _ outcome: InstallOutcome, removing copy: URL?) {
        if let copy { try? FileManager.default.removeItem(at: copy) }
        log.info("install \(id): \(String(describing: outcome), privacy: .public)")
        #if DEBUG
        SessionCheck.event("install \(id) \(outcome)")
        #endif
        installAnswered(id, outcome)
        done()
    }

    // MARK: - An app's request to remove an app

    private func uninstall(_ id: UInt64, text: String) {
        guard let paths, let target = UninstallRequest.target(of: text) else {
            return finishUninstall(id, .failed, why: "the request names no app")
        }
        Task {
            let library: Library
            do {
                library = try Library(paths: paths)
            } catch {
                return finishUninstall(id, .failed, why: "the library did not open: \(error)")
            }
            guard let entry = await library.entries.first(where: { $0.id == target }) else {
                return finishUninstall(id, .failed, why: "not in the library")
            }
            let name = AlertName.plain(entry.displayName).map { "“\($0)”" } ?? "this app"
            // Another app's request says who asks; an app that asks to go itself needs no name twice.
            let from = AlertName.plain(appName).map { "“\($0)”" } ?? "This app"
            let asking = target == package ? "" : "\(from) wants to delete it. "
            let message = asking + "Its installed files and app data will be deleted. The original APK is kept."
            alert(title: "Delete \(name)?", message: message, no: "Cancel", yes: "Delete", destructive: true,
                  event: "uninstall alert \(entry.id)") { [weak self] yes in
                guard let self else { return }
                guard yes else { return finishUninstall(id, .cancelled) }
                Task {
                    do {
                        try await LibraryModel.remove(entry, from: library, paths: paths)
                    } catch {
                        return self.finishUninstall(id, .failed, why: "the library did not remove it: \(error)")
                    }
                    self.finishUninstall(id, .removed)
                    if target == self.package { self.removedItself() }
                }
            }
        }
    }

    private func finishUninstall(_ id: UInt64, _ outcome: UninstallOutcome, why: String? = nil) {
        if let why { log.error("uninstall \(id) failed: \(why, privacy: .public)") }
        log.info("uninstall \(id): \(String(describing: outcome), privacy: .public)")
        #if DEBUG
        SessionCheck.event("uninstall \(id) \(outcome)\(why.map { ": \($0)" } ?? "")")
        #endif
        uninstallAnswered(id, outcome)
        done()
    }

    // MARK: - Minato's alert

    /// Minato's alert in its turn over game mode (SheetTurns), once the scene is active, as the permission alert waits
    /// for the app's return; `answered` gets the user's choice.
    private func alert(title: String, message: String, no: String, yes: String, destructive: Bool = false,
                       event: String, answered: @escaping (Bool) -> Void) {
        SheetTurns.shared.take(self) { [weak self] in
            self?.show(title: title, message: message, no: no, yes: yes, destructive: destructive, event: event,
                       answered: answered)
        }
    }

    private func show(title: String, message: String, no: String, yes: String, destructive: Bool, event: String,
                      answered: @escaping (Bool) -> Void) {
        guard UIApplication.shared.applicationState == .active, let top = OpenPresenter.topController(),
              top.presentedViewController == nil else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
                self?.show(title: title, message: message, no: no, yes: yes, destructive: destructive, event: event,
                           answered: answered)
            }
            return
        }
        var once = false
        let end = { [weak self] (choice: Bool) in
            guard !once, let self else { return }
            once = true
            SheetTurns.shared.give(self)
            answered(choice)
        }
        let alert = UIAlertController(title: title, message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: no, style: .cancel) { _ in end(false) })
        let action = UIAlertAction(title: yes, style: destructive ? .destructive : .default) { _ in end(true) }
        alert.addAction(action)
        // A destructive choice is never the one Return takes, as in Apple's own delete alerts.
        if !destructive { alert.preferredAction = action }
        top.present(alert, animated: true)
        #if DEBUG
        SessionCheck.event(event)
        if SessionCheck.allowsInstalls || SessionCheck.deniesInstalls {
            let choice = SessionCheck.allowsInstalls
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak alert] in
                guard let alert, !once else { return }
                SessionCheck.event("install alert answered \(choice ? yes : no)")
                alert.presentingViewController?.dismiss(animated: true)
                end(choice)
            }
        }
        #endif
    }
}
