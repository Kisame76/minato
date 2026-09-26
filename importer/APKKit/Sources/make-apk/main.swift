// make-apk: packs one of our test apps into an APK signed with the debug key (ADR-0031).
// Usage: make-apk --manifest <AndroidManifest.xml> [--lib <lib*.so>]... [--assets <folder>] [--key <folder>] --out <file.apk>
// Build and run it through tools/make-apk.sh.
import APKPack
import Foundation

let status = MakeAPK.run(Array(CommandLine.arguments.dropFirst()),
                         output: { print($0) },
                         errors: { FileHandle.standardError.write(Data(($0 + "\n").utf8)) })
exit(status)
