import APKFormats
import APKKit
import Foundation

/// Minato's answer to one install session of split APKs, in one line, which tools/formats-reference.sh compares with
/// Android 15's (ADR-0048 b8c): "accept", "ask: <why>" where the library asks first, "missing split" where Android
/// refuses with INSTALL_FAILED_MISSING_SPLIT and the library shows "Needs split APKs", or "refuse: <reason>".
enum SessionAnswer {
    /// `--set <apk>…` checks the APKs as one session; `--join <installed apk>… -- <split apk>…` adds the splits to the
    /// installed APKs first, as `adb install-multiple -p` does.
    static func run(_ mode: String, _ paths: [String]) -> Int32 {
        let separator = paths.firstIndex(of: "--")
        guard !paths.isEmpty, (mode == "--join") == (separator != nil) else {
            FileHandle.standardError.write(Data(
                "usage: apk-inspect --set <file.apk>...\n       apk-inspect --join <installed.apk>... -- <split.apk>...\n".utf8))
            return 2
        }
        do {
            if let separator {
                let installed = try paths[..<separator].map(member)
                let splits = try paths[(separator + 1)...].map(member)
                guard let joined = SetChecks.joining(splits, installed: installed) else {
                    print("refuse: the splits cannot join the installed version: no base, or another package, version code or signer")
                    return 0
                }
                print(answer(SetChecks.check(joined)))
            } else {
                print(answer(SetChecks.check(try paths.map(member))))
            }
        } catch {
            print("refuse: \(error)")
        }
        return 0
    }

    static func member(_ path: String) throws -> SetMember {
        let url = URL(fileURLWithPath: path)
        return try SetMember.read(ZipArchive(url: url), fileName: url.lastPathComponent, sha256: url.lastPathComponent)
    }

    static func answer(_ result: Result<CheckedSet, SetRefusal>) -> String {
        switch result {
        case .failure(let refusal):
            return "refuse: \(refusal.reason)"
        case .success(let set):
            // Splits without their base: the library refuses them unless they join the installed version (decision 7).
            guard set.base != nil else { return "refuse: no base APK: splits without their base only join an installed version" }
            if set.missingSplit { return "missing split" }
            return switch set.signing {
            case .verified: "accept"
            case .unsigned: "ask: unsigned"
            case .unverified(let reason): "ask: cannot verify any APK: \(reason)"
            case .partlyVerified(_, let files): "ask: cannot verify \(files.map(\.fileName).joined(separator: ", "))"
            }
        }
    }
}
