import APKRuntime
import APKSession
import AVFAudio
import AVFoundation
import Foundation
import UIKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "permissions")

/// The guest's permission requests over game mode (ADR-0063 decision 2, ADR-0064 decision 2): PermissionDesk's steps,
/// with Minato's own alert in its turn over game mode (SheetTurns), iOS's record prompt after a yes there, and the
/// answers through apkrt_permission_answer, which records the grant in the runtime and tells the guest. The app's
/// records go to its permissions file after each answer.
@MainActor
final class PermissionPresenter {
    private var desk: PermissionDesk
    private let file: URL?
    private let appName: String
    private weak var alert: UIAlertController?
    private var shownID: UInt64?

    init(file: URL?, appName: String) {
        self.file = file
        self.appName = appName
        desk = PermissionDesk(records: file.map(PermissionRecords.load) ?? PermissionRecords())
    }

    /// iOS's answer for a group, which is Minato's for every app.
    private static func ios(_ group: PermissionGroup) -> PermissionDesk.IOSState {
        switch group {
        case .location: return LocationHost.shared.state
        case .notifications: return NotificationHost.shared.state
        case .camera:
            switch AVCaptureDevice.authorizationStatus(for: .video) {
            case .authorized: return .granted
            case .notDetermined: return .notDetermined
            default: return .denied
            }
        case .microphone:
            switch AVAudioApplication.shared.recordPermission {
            case .granted: return .granted
            case .denied: return .denied
            default: return .notDetermined
            }
        }
    }

    /// Before the guest runs: the stored grant that iOS still gives, so the app's first check finds it.
    func seed() {
        run(desk.seeds(ios: Self.ios))
    }

    /// The scene is active again: a grant iOS took back in Settings becomes a denial, once iOS's answer for
    /// notifications, which comes through a callback, is read again.
    func foreground() {
        NotificationHost.shared.refresh {
            Task { @MainActor [weak self] in
                guard let self else { return }
                run(desk.foreground(ios: Self.ios))
            }
        }
    }

    func requested(id: UInt64, text: String) {
        run(desk.requested(id: id, text: text, ios: Self.ios))
    }

    func cancelled(id: UInt64) {
        run(desk.cancelled(id: id, ios: Self.ios))
    }

    private func run(_ steps: [PermissionDesk.Step]) {
        for step in steps {
            switch step {
            case .answer(let id, let group, let outcome):
                log.info("permission \(id) group \(group): \(outcome == 0 ? "denied" : "granted", privacy: .public)")
                #if DEBUG
                SessionCheck.event("permission \(id) group \(group) outcome \(outcome)")
                #endif
                apkrt_permission_answer(UInt32(truncatingIfNeeded: id), group, outcome)
                save()
            case .ask(let id, let group):
                shownID = id
                SheetTurns.shared.take(self) { [weak self] in self?.show(id, group) }
            case .askIOS(let id, .microphone):
                AVAudioApplication.requestRecordPermission { granted in
                    Task { @MainActor [weak self] in self?.iosAnswered(id, granted ? .granted : .denied) }
                }
            case .askIOS(let id, .camera):
                AVCaptureDevice.requestAccess(for: .video) { granted in
                    Task { @MainActor [weak self] in self?.iosAnswered(id, granted ? .granted : .denied) }
                }
            case .askIOS(let id, .notifications):
                NotificationHost.shared.requestAuthorization { [weak self] state in self?.iosAnswered(id, state) }
            case .askIOS(let id, .location):
                LocationHost.shared.requestAuthorization { [weak self] state in self?.iosAnswered(id, state) }
            case .dismiss(let id):
                guard shownID == id else { continue }
                shownID = nil
                alert?.presentingViewController?.dismiss(animated: true)
                SheetTurns.shared.give(self)
            }
        }
    }

    private func iosAnswered(_ id: UInt64, _ state: PermissionDesk.IOSState) {
        run(desk.iosAnswered(id: id, state: state, ios: Self.ios))
    }

    private func save() {
        guard let file else { return }
        do {
            try desk.records.save(to: file)
        } catch {
            log.error("the permission records were not saved: \(error.localizedDescription, privacy: .public)")
        }
    }

    /// Minato's alert, once nothing of ours is on its way in or out over game mode; it waits while the scene is not
    /// active, as Android's dialog comes with the app's return.
    private func show(_ id: UInt64, _ group: PermissionGroup) {
        guard shownID == id else { return }
        guard UIApplication.shared.applicationState == .active, let top = OpenPresenter.topController(),
              top.presentedViewController == nil else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in self?.show(id, group) }
            return
        }
        let alert = UIAlertController(title: Texts.permissionTitle(appName, group: group), message: nil,
                                      preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Don’t Allow", style: .cancel) { [weak self] _ in
            self?.ended(id, allowed: false)
        })
        let allow = UIAlertAction(title: "Allow", style: .default) { [weak self] _ in self?.ended(id, allowed: true) }
        alert.addAction(allow)
        alert.preferredAction = allow
        self.alert = alert
        top.present(alert, animated: true)
        #if DEBUG
        SessionCheck.event("permission alert \(alert.title ?? "")")
        if SessionCheck.allowsPermissions {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self, weak alert] in
                guard let self, let alert, self.shownID == id else { return }
                SessionCheck.event("permission allowed")
                alert.presentingViewController?.dismiss(animated: true)
                self.ended(id, allowed: true)
            }
        }
        #endif
    }

    /// The user answered Minato's alert for request `id`.
    private func ended(_ id: UInt64, allowed: Bool) {
        guard shownID == id else { return }
        shownID = nil
        SheetTurns.shared.give(self)
        run(allowed ? desk.allowed(id: id, ios: Self.ios) : desk.denied(id: id, ios: Self.ios))
    }
}
