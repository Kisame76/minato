import APKKit
import APKLibrary
import APKSession
import Foundation

/// User-facing wording for everything APKKit reports.
enum Texts {
    static func status(of entry: LibraryEntry) -> LibraryItem.Status {
        switch entry.state {
        case .ready:
            return .ready
        case .needsSplits:
            return .needsSplits
        case .incompatible:
            let blocking = entry.report?.compatibility.issues.first(where: isBlocking)
            return .incompatible(blocking.map(issue) ?? "Unknown reason")
        case .invalid(let reason):
            return .invalid(invalidReason(reason))
        }
    }

    static func statusLine(_ status: LibraryItem.Status) -> String {
        switch status {
        case .new: "New"
        case .installing: "Installing…"
        case .updating: "Updating…"
        case .ready: "Ready"
        case .needsSplits: "Needs split APKs"
        case .incompatible(let reason): "Incompatible: \(reason)"
        case .invalid(let reason): "Not a valid APK: \(reason)"
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
        }
    }

    private static func updating(_ entry: LibraryEntry) -> (title: String, message: String) {
        ("“\(entry.displayName)” Is Updating", "Minato is signing this app’s code with your new certificate. Try again in a moment.")
    }

    /// Why an app cannot start, by its state.
    static func launchProblem(of entry: LibraryEntry) -> (title: String, message: String) {
        switch Texts.status(of: entry) {
        case .ready where entry.report?.runtime == .art:
            ("Can’t Open “\(entry.displayName)” Yet", "This app contains Java/Kotlin code and needs ART, which comes in milestone M4.")
        case .needsSplits:
            ("Can’t Open “\(entry.displayName)”", "This app needs its split APKs. Importing split sets comes in milestone M5.")
        case .incompatible(let reason):
            ("Can’t Open “\(entry.displayName)”", reason + ".")
        case .invalid(let reason):
            ("Can’t Open “\(entry.displayName)”", "This is not a valid APK: \(reason).")
        case .ready, .new, .installing, .updating:
            // A ready app without a report; an entry's status is never one of the other three.
            ("Can’t Open “\(entry.displayName)”", "Import it again.")
        }
    }

    static func isBlocking(_ issue: Compatibility.Issue) -> Bool {
        switch issue {
        case .isSplitAPK, .only32Bit, .noARM64, .wrongArchitecture, .conversionFailed: true
        default: false
        }
    }

    static func issue(_ issue: Compatibility.Issue) -> String {
        switch issue {
        case .isSplitAPK(let name):
            "Split APK “\(name)”, only usable together with its base APK (comes in M5)"
        case .only32Bit(let abis):
            "Only 32-bit code (\(abis.joined(separator: ", "))), which iPhones cannot run since the A11"
        case .noARM64(let abis):
            "No ARM code, only \(abis.joined(separator: ", "))"
        case .wrongArchitecture(let library):
            "\(library) is in the arm64 folder but is not ARM64 code"
        case .conversionFailed(let library, let reason):
            "\(library) cannot run: \(reason)"
        case .needsART:
            "Contains Java/Kotlin code and needs ART (milestone M4)"
        case .requiresGLES(let version):
            "Requires OpenGL ES \(glesVersion(version)); ANGLE on Metal provides at most 3.0"
        case .pageAligned4K(let libraries):
            "\(libraries.count) \(libraries.count == 1 ? "library is" : "libraries are") built for 4 KB pages only (matters in M2)"
        case .usesLicenseCheck:
            "Uses Google’s license check (LVL), which we do not bypass"
        case .unreadableLibrary(let library):
            "\(library) cannot be read"
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
        case .art: "Java/Kotlin, needs ART"
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
