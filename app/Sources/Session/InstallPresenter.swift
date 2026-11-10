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
/// "Open with" during a session does (ADR-0018). The answers: granted or denied; ready, failed or cancelled.
@MainActor
final class InstallPresenter {
    enum Request {
        case askSources(id: UInt64, text: String)
        case confirm(id: UInt64, text: String)
    }

    private let package: String
    private let appName: String
    private let dataFolder: URL?
    private let paths: LibraryPaths?
    private let sourcesAnswered: (UInt64, Bool) -> Void
    private let installAnswered: (UInt64, InstallOutcome) -> Void
    private var waiting: [Request] = []
    private var busy = false

    init(package: String, appName: String, dataFolder: URL?, paths: LibraryPaths?,
         sourcesAnswered: @escaping (UInt64, Bool) -> Void,
         installAnswered: @escaping (UInt64, InstallOutcome) -> Void) {
        self.package = package
        self.appName = appName
        self.dataFolder = dataFolder
        self.paths = paths
        self.sourcesAnswered = sourcesAnswered
        self.installAnswered = installAnswered
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
              let components = InstallFile.components(of: request.path, package: package) else {
            return refuse(id, "the request does not name a file of the app's own data")
        }
        let copy = FileManager.default.temporaryDirectory.appending(path: "install-\(UUID().uuidString).apk")
        Task {
            let check: Result<InstallCheck, Error> = await Task.detached {
                do {
                    try InstallFile.copy(components, below: dataFolder, to: copy)
                    return .success(try await Library(paths: paths).checkForInstall(copy))
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
                            _ = try await Library(paths: paths).adopt(copy)
                            finish(id, .ready, removing: copy)
                        } catch {
                            refuse(id, "the library did not take it: \(error)", removing: copy)
                        }
                    }
                }
            }
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

    // MARK: - Minato's alert

    /// Minato's alert in its turn over game mode (SheetTurns), once the scene is active, as the permission alert waits
    /// for the app's return; `answered` gets the user's choice.
    private func alert(title: String, message: String, no: String, yes: String, event: String,
                       answered: @escaping (Bool) -> Void) {
        SheetTurns.shared.take(self) { [weak self] in
            self?.show(title: title, message: message, no: no, yes: yes, event: event, answered: answered)
        }
    }

    private func show(title: String, message: String, no: String, yes: String, event: String,
                      answered: @escaping (Bool) -> Void) {
        guard UIApplication.shared.applicationState == .active, let top = OpenPresenter.topController(),
              top.presentedViewController == nil else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
                self?.show(title: title, message: message, no: no, yes: yes, event: event, answered: answered)
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
        let action = UIAlertAction(title: yes, style: .default) { _ in end(true) }
        alert.addAction(action)
        alert.preferredAction = action
        top.present(alert, animated: true)
        #if DEBUG
        SessionCheck.event(event)
        if SessionCheck.allowsInstalls {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak alert] in
                guard let alert, !once else { return }
                SessionCheck.event("install alert answered \(yes)")
                alert.presentingViewController?.dismiss(animated: true)
                end(true)
            }
        }
        #endif
    }
}
