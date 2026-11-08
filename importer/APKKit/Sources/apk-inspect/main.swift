// Development tool: prints what APKKit reads from an APK. With --signers it writes the APK's signers.json
// instead, as the library writes it into a version's folder (ADR-0034 decision 2), for the guest's
// PackageManager on the Mac (ADR-0039 b3.5b1): what the signature check proved. An unsigned or unverified APK
// gets none, and a file there is removed; one whose check failed, which the library would not install, exits 1.
// With --set or --join it prints Minato's answer to one install session of split APKs instead (SessionAnswer.swift).
// Usage: swift run apk-inspect [--json] <file.apk>
//        swift run apk-inspect --signers <signers.json> <file.apk>
//        swift run apk-inspect --set <file.apk>...
//        swift run apk-inspect --join <installed.apk>... -- <split.apk>...
import APKKit
import Foundation

var arguments = Array(CommandLine.arguments.dropFirst())
if let mode = arguments.first, mode == "--set" || mode == "--join" {
    exit(SessionAnswer.run(mode, Array(arguments.dropFirst())))
}
let asJSON = arguments.first == "--json"
if asJSON { arguments.removeFirst() }
var signersPath: String?
if arguments.first == "--signers", arguments.count == 3 {
    signersPath = arguments[1]
    arguments.removeFirst(2)
}
guard arguments.count == 1 else {
    FileHandle.standardError.write(Data(
        "usage: apk-inspect [--json] <file.apk>\n       apk-inspect --signers <signers.json> <file.apk>\n".utf8))
    exit(2)
}

do {
    let start = Date()
    let archive = try ZipArchive(url: URL(fileURLWithPath: arguments[0]))
    let report = try APKAnalyzer.analyze(archive, preferredLanguages: ["de", "en"])
    if let signersPath {
        let url = URL(fileURLWithPath: signersPath)
        try? FileManager.default.removeItem(at: url)
        switch report.signature.check {
        case .verified?:
            // A verified check carries its proof.
            if let proof = report.signature.proof {
                try SignersFile.content(of: proof).write(to: url, options: .atomic)
            }
        case .failed(let failure)?:
            FileHandle.standardError.write(Data("error: the signature check failed: \(failure.detail)\n".utf8))
            exit(1)
        case let check:
            print("\(check.map { "\($0)" } ?? "not checked"): no \(SignersFile.fileName)")
        }
        exit(0)
    }
    let elapsed = Date().timeIntervalSince(start)

    if asJSON {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        print(String(decoding: try encoder.encode(report), as: UTF8.self))
        exit(0)
    }

    print("package:        \(report.packageName)")
    print("label:          \(report.label)")
    print("version:        \(report.versionName ?? "-") (\(report.versionCode))")
    print("sdk:            min \(report.minSdk), target \(report.targetSdk)")
    print("launcher:       \(report.launcherActivity ?? "-")")
    print("runtime:        \(report.runtime)")
    print("java code:      \(report.javaCode.map { $0 ? "yes" : "no" } ?? "-"), starts on "
          + (report.startsOnJavaPath ? "app_process" : "activity-host"))
    print("engine:         \(report.engine ?? "-")")
    print("java loads:     \(report.javaLibraries.map { $0.joined(separator: ", ") } ?? "-")")
    print("ABIs:           \(report.abis.joined(separator: ", ")) -> \(report.selectedABI ?? "none")")
    print("GLES/Vulkan:    \(report.requiredGLESVersion.map { String(format: "0x%05x", $0) } ?? "-") / \(report.requiredVulkanVersion.map { String(format: "0x%x", $0) } ?? "-")")
    print("play:           \(report.playDependencies.map(\.rawValue).joined(separator: ", "))")
    print("icon:           \(report.iconPath ?? "-")")
    print("signature:      \(report.signature.schemes.map(\.rawValue).joined(separator: ", ")), Play stamp: \(report.signature.hasGooglePlayStamp)")
    report.signature.certificates.forEach { print("                \($0.sha256)  \($0.subject ?? "?")") }
    print("signature check: \(report.signature.check.map { "\($0)" } ?? "none")")
    print("status:         \(report.compatibility.status.rawValue)")
    report.compatibility.issues.forEach { print("  issue:        \($0)") }
    print("native libraries (\(report.nativeLibraries.count)):")
    for library in report.nativeLibraries {
        let elf = library.elf.map { "AArch64: \($0.isAArch64), LOAD \($0.loadSegmentCount), align \($0.pageAlignment), TLS \($0.hasTLS)" } ?? (library.problem ?? "?")
        print("  \(library.name)  \(library.size) B  \(library.isCompressed ? "deflated" : "stored")  \(elf)")
    }
    print(String(format: "analysed in %.2f s", elapsed))
} catch {
    FileHandle.standardError.write(Data("error: \(error)\n".utf8))
    exit(1)
}
