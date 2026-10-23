import APKSession
import MessageUI
import SafariServices
import UIKit

/// ADR-0062's host: shows what the guest asks to open over game mode, one request at a time (OpenQueue), and answers
/// each once. http and https in SFSafariViewController inside Minato, so back returns to the app; mail and text
/// messages in their composers when the device can send them, else by iOS's open; tel and maps by iOS's open, which
/// asks the user where iOS asks; a share in UIActivityViewController with copies of the share folder's files.
@MainActor
final class OpenPresenter: NSObject {
    /// One presentation of a request: its ID, a token of its own, as the guest may use an ID again after a cancel,
    /// and its controller, none for iOS's open, which shows nothing of ours.
    private struct Shown {
        let id: UInt64
        let token: UInt64
        var controller: UIViewController?
    }

    private var queue = OpenQueue()
    private var shown: Shown?
    private var tokens: UInt64 = 0
    /// The app's data folder, from the host's own records.
    private let dataFolder: URL?
    /// The share's copies of the request shown, removed when it ends.
    private var shareFolder: URL?
    private let answer: (UInt64, OpenOutcome) -> Void

    static let sharePrefix = "share-"

    init(dataFolder: URL?, answer: @escaping (UInt64, OpenOutcome) -> Void) {
        self.dataFolder = dataFolder
        self.answer = answer
        super.init()
        Self.removeOldShareFolders()
    }

    /// The guest's request `id` with the runtime's checked text.
    func requested(id: UInt64, text: String, inBackground: Bool) {
        run(queue.requested(id: id, text: text, inBackground: inBackground,
                            canSendMail: MFMailComposeViewController.canSendMail(),
                            canSendText: MFMessageComposeViewController.canSendText())[...])
    }

    /// The guest no longer wants request `id`.
    func cancelled(id: UInt64) {
        run(queue.cancelled(id: id)[...])
    }

    /// The presentation `token` ended with `outcome`; nothing for one that is no longer shown.
    private func ended(_ token: UInt64, _ outcome: OpenOutcome) {
        guard let shown, shown.token == token else { return }
        self.shown = nil
        removeShareFolder()
        run(queue.ended(id: shown.id, outcome)[...])
    }

    /// Runs `steps` in order; a dismissal runs the steps after it once its controller is gone, so that the next one
    /// is not presented over a controller on its way out, which UIKit would drop without a word.
    private func run(_ steps: ArraySlice<OpenQueue.Step>) {
        var rest = steps
        while let step = rest.popFirst() {
            switch step {
            case .answer(let id, let outcome):
                answer(id, outcome)
            case .dismiss(let id):
                guard let shown, shown.id == id else { continue }
                self.shown = nil
                removeShareFolder()
                if let presenting = shown.controller?.presentingViewController {
                    presenting.dismiss(animated: true) { [weak self] in self?.run(rest) }
                    return
                }
            case .present(let id, let action):
                present(id, action)
            }
        }
    }

    private func present(_ id: UInt64, _ action: OpenAction) {
        tokens += 1
        let token = tokens
        shown = Shown(id: id, token: token)
        guard UIApplication.shared.applicationState != .background else { return ended(token, .background) }
        switch action {
        case .systemOpen(let url):
            UIApplication.shared.open(url) { [weak self] opened in self?.ended(token, opened ? .shown : .noHandler) }
        case .browser(let url):
            // SFSafariViewController raises for any other URL; OpenRequest only makes these.
            guard url.scheme == "http" || url.scheme == "https", url.host() != nil else { return ended(token, .refused) }
            let browser = SFSafariViewController(url: url)
            browser.delegate = self
            show(browser, token)
        case .mailComposer(let draft):
            let composer = MFMailComposeViewController()
            composer.mailComposeDelegate = self
            composer.setToRecipients(draft.to)
            composer.setCcRecipients(draft.cc)
            composer.setBccRecipients(draft.bcc)
            composer.setSubject(draft.subject)
            composer.setMessageBody(draft.body, isHTML: false)
            show(composer, token)
        case .messageComposer(let draft):
            let composer = MFMessageComposeViewController()
            composer.messageComposeDelegate = self
            composer.recipients = draft.recipients
            composer.body = draft.body
            show(composer, token)
        case .shareSheet(let request):
            share(request, id: id, token: token)
        }
    }

    /// Presents `controller` for presentation `token` over the top controller; a present that UIKit drops, or one
    /// without a scene in front, ends as no handler.
    private func show(_ controller: UIViewController, _ token: UInt64) {
        guard shown?.token == token else { return }
        guard let top = Self.topController() else { return ended(token, .noHandler) }
        shown?.controller = controller
        top.present(controller, animated: true)
        DispatchQueue.main.async { [weak self] in
            if controller.presentingViewController == nil, !controller.isBeingPresented { self?.ended(token, .noHandler) }
        }
    }

    /// A share: the files copied off the main thread (a clone where the volume can, else up to ShareFiles' limits),
    /// then the sheet with the subject, the text and the copies; a refused copy ends the request as refused.
    private func share(_ request: ShareRequest, id: UInt64, token: UInt64) {
        guard let dataFolder else { return ended(token, .refused) }
        let folder = FileManager.default.temporaryDirectory.appending(path: "\(Self.sharePrefix)\(UUID().uuidString)")
        guard (try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)) != nil else {
            return ended(token, .refused)
        }
        shareFolder = folder
        let files = request.files
        Task.detached(priority: .userInitiated) {
            let copies = ShareFiles.copy(dataFolder: dataFolder, id: id, files: files, into: folder)
            await MainActor.run { [weak self] in
                guard let self, self.shown?.token == token else {
                    try? FileManager.default.removeItem(at: folder)
                    return
                }
                guard let copies else { return self.ended(token, .refused) }
                self.showShareSheet(request, copies: copies, token: token)
            }
        }
    }

    private func showShareSheet(_ request: ShareRequest, copies: [URL], token: UInt64) {
        var items: [Any] = copies
        if !request.text.isEmpty || !request.subject.isEmpty {
            items.insert(ShareText(subject: request.subject, text: request.text), at: 0)
        }
        guard !items.isEmpty else { return ended(token, .refused) }
        let sheet = UIActivityViewController(activityItems: items, applicationActivities: nil)
        sheet.completionWithItemsHandler = { [weak self] _, completed, _, _ in
            self?.ended(token, completed ? .shown : .cancelled)
        }
        if let top = Self.topController() {
            sheet.popoverPresentationController?.sourceView = top.view
            sheet.popoverPresentationController?.sourceRect = CGRect(x: top.view.bounds.midX, y: top.view.bounds.midY,
                                                                     width: 0, height: 0)
        }
        show(sheet, token)
    }

    private func removeShareFolder() {
        guard let shareFolder else { return }
        try? FileManager.default.removeItem(at: shareFolder)
        self.shareFolder = nil
    }

    /// The copies a share of an earlier process left when it ended with its sheet shown; one session per process, so
    /// none of them is in use.
    private static func removeOldShareFolders() {
        let temporary = FileManager.default.temporaryDirectory
        for name in (try? FileManager.default.contentsOfDirectory(atPath: temporary.path)) ?? []
        where name.hasPrefix(sharePrefix) {
            try? FileManager.default.removeItem(at: temporary.appending(path: name))
        }
    }

    /// The controller on top of the foreground scene's key window, below one on its way out.
    private static func topController() -> UIViewController? {
        let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            .first { $0.activationState == .foregroundActive || $0.activationState == .foregroundInactive }
        var top = scene?.keyWindow?.rootViewController
        while let presented = top?.presentedViewController, !presented.isBeingDismissed { top = presented }
        return top
    }

    /// The token of the presentation whose controller is `controller`.
    private func token(of controller: UIViewController) -> UInt64? {
        guard let shown, shown.controller === controller else { return nil }
        return shown.token
    }
}

extension OpenPresenter: @MainActor SFSafariViewControllerDelegate {
    func safariViewControllerDidFinish(_ controller: SFSafariViewController) {
        guard let token = token(of: controller) else { return }
        // Safari's own dismissal is under way; the next present waits for it.
        DispatchQueue.main.async { [weak self] in self?.ended(token, .shown) }
    }
}

extension OpenPresenter: @MainActor MFMailComposeViewControllerDelegate {
    func mailComposeController(_ controller: MFMailComposeViewController, didFinishWith result: MFMailComposeResult,
                               error: (any Error)?) {
        let token = token(of: controller)
        controller.dismiss(animated: true) { [weak self] in
            if let token { self?.ended(token, result == .sent || result == .saved ? .shown : .cancelled) }
        }
    }
}

extension OpenPresenter: @MainActor MFMessageComposeViewControllerDelegate {
    func messageComposeViewController(_ controller: MFMessageComposeViewController,
                                      didFinishWith result: MessageComposeResult) {
        let token = token(of: controller)
        controller.dismiss(animated: true) { [weak self] in
            if let token { self?.ended(token, result == .sent ? .shown : .cancelled) }
        }
    }
}

/// A share's text with its subject, which Mail takes for the subject line (EXTRA_SUBJECT).
private final class ShareText: NSObject, UIActivityItemSource {
    let subject: String
    let text: String

    init(subject: String, text: String) {
        self.subject = subject
        self.text = text
    }

    func activityViewControllerPlaceholderItem(_ activityViewController: UIActivityViewController) -> Any { text }

    func activityViewController(_ activityViewController: UIActivityViewController,
                                itemForActivityType activityType: UIActivity.ActivityType?) -> Any? { text }

    func activityViewController(_ activityViewController: UIActivityViewController,
                                subjectForActivityType activityType: UIActivity.ActivityType?) -> String { subject }
}
