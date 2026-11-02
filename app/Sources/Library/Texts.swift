import APKKit
import APKLibrary
import APKSession
import Foundation

/// User-facing wording for everything APKKit reports.
enum Texts {
    static func status(of entry: LibraryEntry) -> LibraryItem.Status {
        switch entry.state {
        case .ready:
            let degraded = entry.degradedLibraries
            return degraded.isEmpty ? .ready : .readyWithout(degraded)
        case .needsSplits:
            return .needsSplits
        case .incompatible:
            let blocking = entry.report?.compatibility.issues.first(where: isBlocking)
            return .incompatible(blocking.map(issue) ?? "Unknown reason")
        case .invalid(let reason):
            // A set that was not installed keeps its text to show; a file that is no APK, an error's description.
            return entry.refusal.map(LibraryItem.Status.notInstalled) ?? .invalid(invalidReason(reason))
        }
    }

    static func statusLine(_ status: LibraryItem.Status) -> String {
        switch status {
        case .new: "New"
        case .installing: "Installing…"
        case .updating: "Updating…"
        case .ready: "Ready"
        case .readyWithout(let libraries):
            libraries.count == 1 ? "Ready, without \(libraries[0])" : "Ready, without \(libraries.count) libraries"
        case .needsSplits: "Needs split APKs"
        case .incompatible(let reason): "Incompatible: \(reason)"
        case .invalid(let reason): "Not a valid APK: \(reason)"
        case .notInstalled(let reason): "Not installed: \(reason)"
        }
    }

    /// Why tapping an app does not start it (ADR-0032, decision 3).
    static func launchProblem(of entry: LibraryEntry, reason: LaunchDecision.Reason) -> (title: String, message: String) {
        reason == .updating ? updating(entry) : launchProblem(of: entry)
    }

    /// Why a launch request led to the library instead of game mode (ADR-0032, decision 1).
    static func launchProblem(_ problem: LaunchMode.Problem) -> (title: String, message: String) {
        switch problem {
        case .appNotFound:
            ("Can’t Open the App", "It is no longer in the library.")
        case .appNotReady(let entry):
            launchProblem(of: entry)
        case .quitUnexpectedly(let notice):
            ("“\(notice.name)” quit unexpectedly.", lastSessionCause(notice.cause))
        }
    }

    /// The signal that ended the app, or why it could not start (ADR-0032, decision 8).
    private static func lastSessionCause(_ cause: LastSessionNotice.Cause) -> String {
        switch cause {
        case .signal(let number):
            "It ended with signal \(number)" + (LastSessionNotice.signalName(number).map { " (\($0))" } ?? "") + "."
        case .failure(let reason):
            "It could not start: \(reason)"
        case .endedByIOS:
            "iOS may have closed it because it used too much memory."
        case .exitStatus(let status):
            "It ended with status \(status)."
        }
    }

    private static func updating(_ entry: LibraryEntry) -> (title: String, message: String) {
        ("“\(entry.displayName)” Is Updating", "Minato is signing this app’s code with your new certificate. Try again in a moment.")
    }

    /// Why an app cannot start, by its state.
    static func launchProblem(of entry: LibraryEntry) -> (title: String, message: String) {
        switch Texts.status(of: entry) {
        case .needsSplits:
            ("Can’t Open “\(entry.displayName)”", "This app needs its split APKs. Pick all of its APKs with +, or their folder.")
        case .incompatible(let reason):
            ("Can’t Open “\(entry.displayName)”", reason + ".")
        case .invalid(let reason):
            ("Can’t Open “\(entry.displayName)”", "This is not a valid APK: \(reason).")
        case .notInstalled(let reason):
            ("Can’t Open “\(entry.displayName)”", "Not installed: " + (reason.hasSuffix(".") ? reason : reason + "."))
        case .ready, .readyWithout, .new, .installing, .updating:
            // A ready app without a report; an entry's status is never one of the other three.
            ("Can’t Open “\(entry.displayName)”", "Import it again.")
        }
    }

    // MARK: - Updates (ADR-0034, decision 5)

    /// The note after an import that changed an installed app or found it installed already; nil otherwise.
    static func updateNote(_ result: ImportResult) -> String? {
        let name = result.entry.displayName
        let version = versionName(result.report)
        return switch result.decision {
        case .update: "Updated “\(name)” to \(version)."
        case .unchanged: "“\(name)” \(version) is already installed."
        case .reinstall: "Installed “\(name)” \(version) again."
        case .install, .reject, .ask: nil
        }
    }

    /// The note after "Replace".
    static func replacedNote(_ entry: LibraryEntry) -> String {
        "Replaced “\(entry.displayName)” with \(versionName(entry.report))."
    }

    /// A file the installed app keeps its version against: it needs splits or cannot run.
    static func updateRejected(installed: LibraryEntry, report: APKReport?, state: LibraryEntry.State) -> (title: String, message: String) {
        let blocking = report?.compatibility.issues.first(where: isBlocking)
        let reason = switch (state, blocking) {
        case (.needsSplits, _): "needs its split APKs"
        case (_, .signatureInvalid(let failure)?): signatureReason(failure)
        default: blocking.map { "cannot run: " + issue($0) } ?? "cannot run"
        }
        return ("Can’t Update “\(installed.displayName)”",
                "\(versionName(report)) \(reason). Version \(versionName(installed.report)) stays installed.")
    }

    /// The question before an older version, another signer, APKs Minato cannot verify or OBBs with other contents
    /// replace the installed app or its OBBs; every warning together.
    static func updateQuestion(installed: LibraryEntry, report: APKReport, warnings: Set<UpdateWarning>,
                               uncheckedFiles: [String] = [], otherOBBs: [String] = []) -> (title: String, message: String) {
        let name = installed.displayName
        var message: [String] = []
        if warnings.contains(.olderVersion) {
            message.append("\(versionName(report)) is older than the installed \(versionName(installed.report)). "
                + "The data stays, but the older version may not be able to read it.")
        }
        if warnings.contains(.otherSigner) {
            message.append("The new file is signed by someone else, for example as a modified version. Replacing it "
                + "gives it the data of the installed app. To start without that data, delete the app first.")
        }
        if warnings.contains(.uncheckedContent) {
            message.append(uncheckedContent(uncheckedFiles))
        }
        if warnings.contains(.otherOBB) {
            message.append(otherExpansionFiles(otherOBBs))
        }
        let title = warnings.contains(.olderVersion) ? "Replace “\(name)” with an Older Version?" : "Replace “\(name)”?"
        return (title, message.joined(separator: " "))
    }

    // MARK: - Units (ADR-0048 b7b)

    /// The question about a unit's sets that ask (F7): one set as before, several in one alert, "Install" where none it
    /// lists is installed, and "Replace and Install" where it lists installed and new apps.
    static func unitQuestion(_ question: UpdateQuestion) -> (title: String, message: String, confirm: String, decline: String) {
        let replacing = question.sets.filter { $0.installed != nil }.count
        let installing = question.sets.count - replacing
        let (confirm, decline) = switch (replacing, installing) {
        case (0, _): ("Install", "Don’t Install")
        case (_, 0): ("Replace", "Keep Installed")
        default: ("Replace and Install", "Don’t Install")
        }
        guard question.sets.count > 1 else {
            let (title, message) = question.sets.first.map(setQuestion) ?? ("", "")
            return (title, message, confirm, decline)
        }
        let title = replacing > 0 && installing > 0
            ? "Replace \(apps(replacing)) and Install \(installing)?" : "\(confirm) \(apps(question.sets.count))?"
        let message = question.sets.map { "“\($0.name)”: \(setQuestion($0).message)" }
        return (title, message.joined(separator: "\n\n"), confirm, decline)
    }

    /// The alert about a unit's files the installed apps keep their versions against (F7): one as before, several in
    /// one alert with a paragraph each; nil where none is rejected.
    static func updateRejected(_ results: [ImportResult]) -> (title: String, message: String)? {
        let rejected = results.compactMap { result -> (name: String, message: String, title: String)? in
            guard case .reject(let state) = result.decision else { return nil }
            let (title, message) = updateRejected(installed: result.entry, report: result.report, state: state)
            return (result.entry.displayName, message, title)
        }
        guard rejected.count > 1 else { return rejected.first.map { ($0.title, $0.message) } }
        return ("Can’t Update \(apps(rejected.count))", rejected.map { "“\($0.name)”: \($0.message)" }.joined(separator: "\n\n"))
    }

    /// "1 App", "2 Apps".
    private static func apps(_ count: Int) -> String {
        count == 1 ? "1 App" : "\(count) Apps"
    }

    /// One set's question: the update question over an installed app, else whether to install it.
    private static func setQuestion(_ set: UpdateQuestion.Asking) -> (title: String, message: String) {
        if let installed = set.installed {
            return updateQuestion(installed: installed, report: set.report, warnings: set.warnings,
                                  uncheckedFiles: set.uncheckedFiles, otherOBBs: set.otherOBBs)
        }
        return ("Install “\(set.name)”?", uncheckedContent(set.uncheckedFiles))
    }

    /// APKs Minato cannot verify (ADR-0048, step b3), by the names the user picked them under.
    static func uncheckedContent(_ files: [String]) -> String {
        switch files.count {
        case 0: "Minato cannot verify how some of its APKs are signed, so it cannot tell whether their content was changed."
        case 1: "Minato cannot verify how \(files[0]) is signed, so it cannot tell whether its content was changed."
        default: "Minato cannot verify how \(list(files)) are signed, so it cannot tell whether their content was changed."
        }
    }

    /// Installed OBBs an import would replace with other contents (ADR-0049 c2), by name, in App's words.
    static func otherExpansionFiles(_ names: [String]) -> String {
        let quoted = names.map { "“\($0)”" }
        return switch quoted.count {
        case 0: "Its expansion files have different contents from the installed ones, for example from another source. "
            + "Replacing them changes the app’s content."
        case 1: "The expansion file \(quoted[0]) has different contents from the installed one, for example from another "
            + "source. Replacing it changes the app’s content."
        default: "The expansion files \(list(quoted)) have different contents from the installed ones, for example from "
            + "another source. Replacing them changes the app’s content."
        }
    }

    // MARK: - OBBs (ADR-0049 d), in App's words

    /// The question about an OBB no app takes by its name, or the alert where no app can take one.
    static func obbQuestion(_ question: OBBQuestion) -> (title: String, message: String) {
        question.apps.isEmpty
            ? ("No App for “\(question.file)”", "Import the app it belongs to first, then its expansion file.")
            : ("Add “\(question.file)” to an App?", "An expansion file holds extra content for an app. Choose the app it belongs to.")
    }

    /// The note after OBBs came into an app: "“<file>” was added to “<app>”.", and what they replaced; nil where none
    /// came. `file` is the name the user picked where they named the app, else the OBBs' own names are shown.
    static func obbNote(_ result: ImportResult, file: String? = nil) -> String? {
        guard !result.installedOBBs.isEmpty else { return nil }
        let added = file.map { [$0] } ?? result.installedOBBs.map(\.fileName)
        var note = "\(list(added.map { "“\($0)”" })) \(added.count == 1 ? "was" : "were") added to “\(result.entry.displayName)”."
        if !result.replacedOBBs.isEmpty {
            note += " \(added.count == 1 ? "It replaces" : "They replace") \(list(result.replacedOBBs.map { "“\($0)”" }))."
        }
        return note
    }

    /// A picked OBB that could not go in, by its picked name, with why.
    static func obbRefused(_ file: String, app: String, because reason: String) -> String {
        "“\(file)” couldn’t be added to “\(app)”: \(reason)."
    }

    /// The question's apps, for an error after the answer.
    static func subject(of question: UpdateQuestion) -> String {
        question.sets.count == 1 ? "“\(question.sets[0].name)”" : "The \(question.sets.count) apps"
    }

    /// A pick's or a unit's files, for messages: "“a.apk”", or "“a.apk” and 2 more".
    static func files(_ names: [String]) -> String {
        guard let first = names.first else { return "The pick" }
        return names.count == 1 ? "“\(first)”" : "“\(first)” and \(names.count - 1) more"
    }

    /// "a, b and c".
    private static func list(_ items: [String]) -> String {
        guard let last = items.last, items.count > 1 else { return items.joined() }
        return items.dropLast().joined(separator: ", ") + " and " + last
    }

    /// A version's name, or its code when the manifest names none.
    static func versionName(_ report: APKReport?) -> String {
        guard let report else { return "?" }
        return report.versionName.flatMap { $0.isEmpty ? nil : $0 } ?? "version \(report.versionCode)"
    }

    static func isBlocking(_ issue: Compatibility.Issue) -> Bool {
        switch issue {
        case .isSplitAPK, .only32Bit, .noARM64, .wrongArchitecture, .conversionFailed, .signatureInvalid, .olderSDK,
             .sdkReference, .splitChoice: true
        default: false
        }
    }

    static func issue(_ issue: Compatibility.Issue) -> String {
        switch issue {
        case .isSplitAPK(let name):
            "Split APK “\(name)”, only usable together with its base APK. Pick all of the app’s APKs with +, or their folder"
        case .only32Bit(let abis):
            "Only 32-bit code (\(abis.joined(separator: ", "))), which iPhones cannot run since the A11"
        case .noARM64(let abis):
            "No ARM code, only \(abis.joined(separator: ", "))"
        case .wrongArchitecture(let library):
            "\(library) is in the arm64 folder but is not ARM64 code"
        case .conversionFailed(let library, let reason):
            "\(library) cannot run: \(reason)"
        case .signatureInvalid(let failure):
            "The file \(signatureReason(failure))"
        case .olderSDK(let message):
            "Android 15 does not install it: \(message)"
        case .sdkReference(let attribute):
            "Its \(attribute) is a resource reference, which Minato does not resolve"
        case .splitChoice(let reason):
            "Its split APKs do not fit this iPhone: \(reason)"
        case .libraryDegraded(let library, let reason):
            "\(library) will not load, so the app runs without it:\n\(reason)"
        case .libraryCallsStubs(let library, let reason):
            "\(library) \(reason)"
        case .libraryNotChecked(let library, let reason):
            "\(library) could not be checked: \(reason)"
        case .needsART:
            "Contains Java/Kotlin code, which runs interpreted for now and may be slower"
        case .requiresGLES(let version):
            "Requires OpenGL ES \(glesVersion(version)); Minato provides at most 3.1"
        case .pageAligned4K(let libraries):
            "\(libraries.count) \(libraries.count == 1 ? "library is" : "libraries are") built for 4 KB pages "
                + "and laid out for 16 KB pages at import"
        case .usesLicenseCheck:
            "Uses Google’s license check (LVL), which we do not bypass"
        case .unreadableLibrary(let library):
            "\(library) cannot be read"
        }
    }

    /// Why a file's signature check failed (ADR-0034, step d), after the file or its version.
    static func signatureReason(_ failure: SignatureFailure) -> String {
        switch failure.kind {
        case .damaged: "is damaged"
        case .signatureMismatch: "has a signature that does not match"
        case .noV2Signature: "has only a v1 signature, too old for the Android version it targets"
        }
    }

    /// What the signature check found, for the details (ADR-0034, step d).
    static func signatureCheck(_ check: SignatureCheck?) -> String {
        switch check {
        case .verified: "Verified"
        case .unsigned: "Unsigned"
        case .unverified(let reason): "Not verified: \(reason)"
        case .failed(let failure):
            switch failure.kind {
            case .damaged: "Failed: the file is damaged"
            case .signatureMismatch: "Failed: the signature does not match"
            case .noV2Signature: "Failed: only a v1 signature, too old for the Android version the app targets"
            }
        case nil: "Not checked"
        }
    }

    static func invalidReason(_ raw: String) -> String {
        if raw.contains("notAZipArchive") { return "not a ZIP file" }
        if raw.contains("AndroidManifest.xml") { return "no AndroidManifest.xml" }
        if raw.contains("unsafePackageName") { return "invalid package name" }
        if raw.contains("invalidManifest") || raw.contains("invalidBinaryXML") { return "manifest not readable" }
        return "not readable"
    }

    static func glesVersion(_ version: UInt32) -> String {
        "\(version >> 16).\(version & 0xFFFF)"
    }

    static func vulkanVersion(_ version: UInt32) -> String {
        // Android encodes it like VK_MAKE_VERSION: major << 22 | minor << 12 | patch.
        "\(version >> 22).\((version >> 12) & 0x3FF)"
    }

    static func runtime(_ runtime: APKReport.Runtime) -> String {
        switch runtime {
        case .nativeActivity(let library): "NativeActivity (lib\(library).so), no Java"
        case .art: "Java/Kotlin"
        }
    }

    /// The phone a device profile names (runtime/src/properties.cpp's table).
    static func deviceProfile(_ profile: AppProfile.DeviceProfile) -> String {
        switch profile {
        case .pixel: "Pixel 9"
        case .galaxy: "Galaxy S24"
        }
    }

    static func playDependency(_ dependency: APKReport.PlayDependency) -> String {
        switch dependency {
        case .playServices: "Google Play services"
        case .firebase: "Firebase"
        case .billing: "Google Play Billing (in-app purchases)"
        case .licenseCheck: "License check (LVL)"
        case .playGames: "Google Play Games"
        }
    }
}
