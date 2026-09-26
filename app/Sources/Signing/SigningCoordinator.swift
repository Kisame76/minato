import APKLibrary
import APKSession
import APKSigning
import Foundation
import Observation
import UIKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "signing")

/// Keeps the signing certificate right without the user's help (ADR-0023). At launch and whenever
/// the app comes back to the foreground it compares the imported certificate with the one the app
/// is signed with; when one is missing or they differ it fetches the right one: from the file
/// SideStore embeds, without a tap, else through the export of the store that installed the app,
/// which needs one tap in the store. Only that export shows the setup sheet.
@MainActor
@Observable
final class SigningCoordinator {
    enum Activity: Equatable {
        case idle
        case importing
        case waitingForStore(Store)
        case resigning
    }

    enum Problem: Equatable {
        /// Back from the store without a certificate: Cancel, or a store too old for the export.
        case noCallback(Store)
        case storeMissing(Store)
        case importFailed(String)
    }

    private(set) var state: SigningState = .notNeeded
    private(set) var next: NextStep = .none
    private(set) var activity: Activity = .idle
    private(set) var problem: Problem?
    /// The store the setup sheet is about.
    private(set) var setupStore: Store?
    var showsSetup = false

    @ObservationIgnored let records: SigningRecordFile
    @ObservationIgnored let environment: SigningEnvironment
    /// Where converted code is signed again after a certificate change.
    @ObservationIgnored private let library: LibraryModel?
    /// One look at a time: a refresh during a look asks for one more look afterwards.
    @ObservationIgnored private var isLooking = false
    @ObservationIgnored private var needsAnotherLook = false
    @ObservationIgnored private var openedStoreThisLaunch = false
    @ObservationIgnored private var embeddedCertificateFailed = false
    /// Each automatic step runs at most once per launch, even if its outcome cannot be saved or some
    /// entries fail, so that nothing loops: importing the embedded file, the self-test, signing again.
    @ObservationIgnored private var triedEmbedded = false
    @ObservationIgnored private var ranSelfTest = false
    @ObservationIgnored private var ranResign = false
    #if DEBUG
    @ObservationIgnored private var handledCallback = false
    @ObservationIgnored private var simulatedChangeForCheck = false
    #endif

    init(library: LibraryModel?, records: SigningRecordFile = .standard, environment: SigningEnvironment = .current()) {
        self.library = library
        self.records = records
        self.environment = environment
        if SelfTestGuard.endedTheAppLastTime() {
            // A broken signature makes iOS end the process when it runs the test code. Loading
            // nothing by itself in this launch keeps that from happening at every start.
            log.error("The self-test ended the app last time; nothing is loaded by itself in this launch")
            embeddedCertificateFailed = true
            triedEmbedded = true
            ranSelfTest = true
            ranResign = true
            try? records.update { record in
                guard let sha256 = record.sha256 else { return }
                record.selfTest = SelfTestRecord(sha256: sha256, passed: false,
                                                 message: "The self-test ended the app when it ran the signed test code.",
                                                 date: .now)
            }
        }
        // The state the first look will find: a tap in the library can come before that look ends, which
        // waits for the library, busy with an import maybe (ADR-0032, decision 3).
        state = assess(certificateInputs()).state
        #if DEBUG
        CertificateCheck.reportAtLaunchIfRequested(self)
        #endif
    }

    /// Looks again and does what is needed; at launch and on every return to the foreground.
    func refresh() async {
        guard !isLooking else {
            needsAnotherLook = true
            return
        }
        isLooking = true
        repeat {
            needsAnotherLook = false
            await look()
        } while needsAnotherLook
        isLooking = false
        reportForCheck()
    }

    /// Opens the store's export link; the store answers through `handle(_:)`.
    func getCertificate(from store: Store) async {
        problem = nil
        let request = ExportRequest(store: store, created: .now)
        try? records.update { $0.pendingExport = request }
        openedStoreThisLaunch = true
        activity = .waitingForStore(store)
        log.info("Asking \(store.name, privacy: .public) for the certificate")
        if await !UIApplication.shared.open(request.url) {
            activity = .idle
            problem = .storeMissing(store)
        }
    }

    /// A certificate callback from a store; false for any other URL.
    func handle(_ url: URL) -> Bool {
        guard url.scheme?.lowercased() == CertificateCallback.scheme else { return false }
        log.info("Received a certificate callback")
        let pending = records.load().pendingExport
        let callback: CertificateCallback
        do {
            callback = try CertificateCallback.parse(url)
            try callback.check(against: pending, now: .now)
        } catch {
            // Never log the URL or its parts: it holds the private key, and the password comes raw.
            let query = url.query(percentEncoded: true) ?? ""
            let fields = ["request", "cert", "password"].filter { query.contains("\($0)=") }
            log.error("Refused a certificate callback: \(String(describing: error), privacy: .public); \(query.count) characters with \(fields.joined(separator: ", "), privacy: .public)")
            // Nobody asked: a link from elsewhere, not worth the user's attention.
            guard pending != nil else { return true }
            if case .waitingForStore = activity { activity = .idle }
            problem = .importFailed(Self.message(for: error))
            showsSetup = true
            return true
        }
        guard let store = pending?.store else { return true }
        activity = .importing
        Task { await importCallback(callback, from: store) }
        return true
    }

    /// A .p12 file the user picked, from the setup sheet or the Signing screen; the reason when it
    /// was not imported.
    func importFile(_ data: Data, password: String) async -> String? {
        activity = .importing
        let outcome = await Self.importing(data, passwords: [password], source: .file, own: environment.ownSignature)
        activity = .idle
        await refresh()
        if case .failure(let failure) = outcome { return failure.message }
        showsSetup = false
        return nil
    }

    func runSelfTest() async {
        _ = await CertificateImportLane.shared.runSelfTest()
        await refresh()
    }

    /// "Simulate Certificate Change" in the diagnostics: the stored certificate and everything signed
    /// with it count as old, so the whole way runs for real: fetch again, sign again, self-test.
    func simulateChange() async {
        await markEverythingOld()
        embeddedCertificateFailed = false
        triedEmbedded = false
        openedStoreThisLaunch = false
        ranResign = false
        await refresh()
    }

    /// Removes the certificate and what the app remembers about it; the app then gets it again as
    /// at the first launch, from SideStore by itself or through the store's export.
    func removeCertificate() async {
        await CertificateImportLane.shared.removeCertificate(records: records)
        embeddedCertificateFailed = false
        triedEmbedded = false
        ranSelfTest = false
        await refresh()
    }

    func dismissSetup() {
        showsSetup = false
    }

    // MARK: - Looking

    private func look() async {
        switch activity {
        case .importing, .resigning:
            return
        case .waitingForStore(let store):
            // Back from the store. Its callback, if there is one, arrives right after the app is active.
            try? await Task.sleep(for: .seconds(1.5))
            guard activity == .waitingForStore(store) else { return }
            activity = .idle
            problem = .noCallback(store)
            showsSetup = true
            return
        case .idle:
            break
        }
        #if DEBUG
        if CertificateCheck.simulatesChange, !simulatedChangeForCheck {
            simulatedChangeForCheck = true
            await markEverythingOld()
        }
        #endif

        let inputs = await gatherInputs()
        // A callback may have started an import while the library was busy.
        guard activity == .idle else { return }
        (state, next) = assess(inputs)
        if case .valid = state {
            showsSetup = false
            problem = nil
        }
        switch next {
        case .none:
            break
        case .importEmbedded:
            await importEmbedded()
            needsAnotherLook = true
        case .requestExport(let store, let automatically):
            setupStore = store
            if case .storeMissing = problem { problem = nil }
            showsSetup = true
            #if DEBUG
            if let callback = CertificateCheck.callbackToOpen, CertificateCheck.awaitsCallback {
                await UIApplication.shared.open(callback)
            }
            #endif
            if automatically && !checkAwaitsCallback {
                // Long enough to read what happens before the app switches to the store.
                try? await Task.sleep(for: .seconds(1))
                // The user may have closed the sheet, or a callback may have come in meanwhile.
                if showsSetup, activity == .idle { await getCertificate(from: store) }
            }
        case .offerFileImport:
            // No sheet for a build from Xcode: the deploy hand-over brings its certificate.
            if case .store(let store) = inputs.installer {
                setupStore = store
                problem = .storeMissing(store)
                showsSetup = true
            }
        case .runSelfTest:
            guard !ranSelfTest else { break }
            ranSelfTest = true
            _ = await CertificateImportLane.shared.runSelfTest()
            needsAnotherLook = true
        case .resign(let count):
            guard !ranResign, let library, let own = environment.own else { break }
            ranResign = true
            await resign(count, with: library, fingerprint: own.sha256)
            needsAnotherLook = true
        }
    }

    /// What the state depends on, which is read at once: the certificates and the record.
    private func certificateInputs() -> SigningInputs {
        let record = records.load()
        var inputs = SigningInputs(own: environment.own, imported: CodeSigning.storedCertificateInfo(), now: .now)
        inputs.ownCertificateProblem = environment.ownSignature.problem
        inputs.markedStale = record.markedStale
        inputs.selfTest = record.selfTest
        return inputs
    }

    private func gatherInputs() async -> SigningInputs {
        var inputs = certificateInputs()
        inputs.hasEmbeddedCertificate = environment.embeddedCertificate != nil
        inputs.embeddedCertificateFailed = embeddedCertificateFailed
        inputs.installer = environment.installer
        inputs.reachableStores = environment.reachableStores()
        inputs.openedStoreThisLaunch = openedStoreThisLaunch
        if let own = environment.own, let library {
            inputs.pendingResign = entriesToResign(await library.resignCandidates(), current: own.sha256).count
        }
        return inputs
    }

    /// The stored certificate counts as another one, and every entry as signed by an old one.
    private func markEverythingOld() async {
        try? records.update { $0.markedStale = true }
        await library?.forgetSignatures()
    }

    // MARK: - Importing

    private struct Failure: Error {
        let message: String
        let wrongPassword: Bool
    }

    private static func importing(_ data: Data, passwords: [String], source: CertificateSource,
                                  own: OwnSignature) async -> Result<CertificateInfo, Failure> {
        do {
            return .success(try await CertificateImportLane.shared.importCertificate(data, passwords: passwords, source: source,
                                                                                    own: own))
        } catch let error as CertificateImportError {
            return .failure(Failure(message: error.localizedDescription, wrongPassword: error == .wrongPassword))
        } catch {
            return .failure(Failure(message: error.localizedDescription, wrongPassword: false))
        }
    }

    private func importEmbedded() async {
        guard !triedEmbedded, let file = environment.embeddedCertificate, let data = try? Data(contentsOf: file) else {
            // Tried once already in this launch, e.g. imported but not found again in the keychain.
            embeddedCertificateFailed = true
            return
        }
        triedEmbedded = true
        activity = .importing
        // SideStore's file needs no password; the serial is the one SideStore names for it.
        let own = environment.ownSignature
        let outcome = await Self.importing(data, passwords: own.certificate.map { [$0.serialNumber] } ?? [],
                                           source: .sideStoreEmbedded, own: own)
        activity = .idle
        if case .failure(let failure) = outcome {
            embeddedCertificateFailed = true
            log.error("The certificate SideStore embedded did not import: \(failure.message, privacy: .public)")
        }
    }

    private func importCallback(_ callback: CertificateCallback, from store: Store) async {
        let outcome = await Self.importing(callback.certificate, passwords: callback.passwordCandidates,
                                           source: store == .sideStore ? .sideStoreExport : .altStoreExport,
                                           own: environment.ownSignature)
        activity = .idle
        switch outcome {
        case .success:
            problem = nil
            showsSetup = false
        case .failure(let failure) where failure.wrongPassword && store == .altStore:
            problem = .importFailed("The password did not fit. In AltStore, leave the password empty and tap Export again.")
            showsSetup = true
        case .failure(let failure):
            problem = .importFailed(failure.message)
            showsSetup = true
        }
        #if DEBUG
        handledCallback = true
        #endif
        await refresh()
    }

    private static func message(for error: CallbackError) -> String {
        switch error {
        case .noPendingRequest, .wrongRequest, .expired:
            "This answer from the store belongs to an earlier request. Please try again."
        case .notACertificateCallback, .missingField, .badCertificate, .tooLarge:
            "The certificate from the store could not be read. Please try again."
        }
    }

    // MARK: - Signing native code (ADR-0032)

    /// What signs native code now: ad hoc when the app itself is signed ad hoc (Simulator), the certificate
    /// once it is valid and passed its self-test, as signing again waits for (ADR-0023), and nothing
    /// otherwise. Its fingerprint is the one signing again compares with.
    func currentSigner() -> CurrentSigner {
        if environment.ownSignature == .adHoc { return .adHoc }
        guard case .valid(let certificate) = state, let own = environment.own,
              let selfTest = records.load().selfTest, selfTest.sha256 == certificate.sha256, selfTest.passed
        else { return .none }
        return .certificate(sha256: own.sha256)
    }

    /// The converter that signs as `signer` does; nil for no signer, or when the certificate cannot be
    /// taken from the keychain.
    func converter(for signer: CurrentSigner) -> SignerConverter? {
        do {
            switch signer {
            case .certificate: return SignerConverter(signer: try CodeSigning.signer())
            case .adHoc: return SignerConverter(signer: try ContainerSigner(identity: nil))
            case .none: return nil
            }
        } catch {
            log.error("Cannot sign native code: \(error.localizedDescription, privacy: .public)")
            return nil
        }
    }

    /// The converter for native code at import. It signs with the certificate that `currentSigner`
    /// names. Otherwise, and when that certificate cannot be used, it converts ad hoc only to learn
    /// whether the code can run; the import keeps the originals and the first tap signs them.
    func importSigning() -> ImportSigning? {
        let signer = currentSigner()
        if case .certificate(let sha256) = signer, let converter = converter(for: signer) {
            return ImportSigning(converter: converter, fingerprint: sha256)
        }
        guard let checking = converter(for: .adHoc) else { return nil }
        return ImportSigning(converter: checking, fingerprint: nil)
    }

    /// A tap on an app while nothing signs (ADR-0032, decision 3) opens the setup when the certificate
    /// has to be fetched; the Signing Certificate screen explains every other state.
    var fetchesCertificate: Bool {
        switch state {
        case .missing, .changed: true
        default: false
        }
    }

    // MARK: - Signing again

    private func resign(_ count: Int, with library: LibraryModel, fingerprint: String) async {
        let converter: SignerConverter
        do {
            converter = SignerConverter(signer: try CodeSigning.signer())
        } catch {
            log.error("Cannot sign again: \(error.localizedDescription, privacy: .public)")
            return
        }
        activity = .resigning
        let summary = await library.resignAll(fingerprint: fingerprint, with: converter)
        activity = .idle
        let record = ResignRecord(date: .now, entries: summary.entries, files: summary.files, failures: summary.failures)
        try? records.update { $0.lastResign = record }
        log.info("Signed \(summary.files) files of \(summary.entries) of \(count) apps again; \(summary.failures.count) failed")
    }

    // MARK: - tools/certificate-check.sh

    private var checkAwaitsCallback: Bool {
        #if DEBUG
        CertificateCheck.awaitsCallback
        #else
        false
        #endif
    }

    private func reportForCheck() {
        #if DEBUG
        guard activity == .idle, !CertificateCheck.awaitsCallback || handledCallback else { return }
        CertificateCheck.reportIfRequested(self)
        #endif
    }
}
