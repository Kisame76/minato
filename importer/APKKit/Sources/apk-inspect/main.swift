// Development tool: prints what APKKit reads from an APK.
// Usage: swift run apk-inspect [--json] <file.apk>
import APKKit
import Foundation

var arguments = Array(CommandLine.arguments.dropFirst())
let asJSON = arguments.first == "--json"
if asJSON { arguments.removeFirst() }
guard arguments.count == 1 else {
    FileHandle.standardError.write(Data("usage: apk-inspect [--json] <file.apk>\n".utf8))
    exit(2)
}

do {
    let start = Date()
    let archive = try ZipArchive(url: URL(fileURLWithPath: arguments[0]))
    let report = try APKAnalyzer.analyze(archive, preferredLanguages: ["de", "en"])
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
    print("engine:         \(report.engine ?? "-")")
    print("ABIs:           \(report.abis.joined(separator: ", ")) -> \(report.selectedABI ?? "none")")
    print("GLES/Vulkan:    \(report.requiredGLESVersion.map { String(format: "0x%05x", $0) } ?? "-") / \(report.requiredVulkanVersion.map { String(format: "0x%x", $0) } ?? "-")")
    print("play:           \(report.playDependencies.map(\.rawValue).joined(separator: ", "))")
    print("icon:           \(report.iconPath ?? "-")")
    print("signature:      \(report.signature.schemes.map(\.rawValue).joined(separator: ", ")), Play stamp: \(report.signature.hasGooglePlayStamp)")
    report.signature.certificates.forEach { print("                \($0.sha256)  \($0.subject ?? "?")") }
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
