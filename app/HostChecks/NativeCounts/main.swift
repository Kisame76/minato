import APKConvert
import Foundation

// tools/native-counts-check.sh's checks of app/Sources/Signing/DebugNativeCounts.swift, the Debug line of Get Info over
// apkconv_counts_of: the numbers of every ELF original in a folder are added, a container (.dylib) and a file the
// converter cannot count are left out, a folder without a library has no line, and the line holds numbers only.
// Arguments: an ELF file the converter counts.

var failures = 0

func check(_ condition: Bool, _ what: String) {
    print("\(condition ? "ok  " : "FAIL") \(what)")
    if !condition { failures += 1 }
}

let elf = URL(fileURLWithPath: CommandLine.arguments[1])
let folder = FileManager.default.temporaryDirectory
    .appending(path: "native-counts-\(getpid())", directoryHint: .isDirectory)
try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
defer { try? FileManager.default.removeItem(at: folder) }

check(DebugNativeCounts.line(libraries: folder) == nil, "a folder without a library has no line")

for name in ["libfirst.so", "libsecond.so"] {
    try FileManager.default.copyItem(at: elf, to: folder.appending(path: name))
}
try Data("container".utf8).write(to: folder.appending(path: "libfirst.so.dylib"))
try Data("not an ELF file".utf8).write(to: folder.appending(path: "libbroken.so"))

let real = apkconv_counts_of_single(elf)
let line = DebugNativeCounts.line(libraries: folder)
check(line?.hasPrefix("2 libraries: ") == true, "two libraries are counted, the container and the broken file are not")
check(line == DebugNativeCounts.text(doubled(real), libraries: 2), "the numbers of the libraries are added")
check(real.svc_rewritten > 0 && line?.contains("svc \(real.svc_rewritten * 2) rewritten") == true,
      "the svc words are added")

let names = line ?? ""
check(!names.contains("libfirst") && !names.contains("libsecond") && !names.contains("0x"),
      "the line names no library and no address")

let one = DebugNativeCounts.line(libraries: folder, count: { $0.lastPathComponent == "libfirst.so" ? real : nil })
check(one?.hasPrefix("1 library: ") == true, "one library says library")

// Every number of apkconv_counts is added and shown, each in its place: a distinct value per field, two libraries.
let each = apkconv_counts(svc_rewritten: 1, svc_data: 2, svc_in_data: 3, undefined_words: 4, x18_islands: 5,
                          renames_by_symbol: 6, renames_by_unwind: 7)
let pair: Set = ["libfirst.so", "libsecond.so"]
let summed = DebugNativeCounts.line(libraries: folder, count: { pair.contains($0.lastPathComponent) ? each : nil })
check(summed == "2 libraries: svc 2 rewritten, 4 kept as data, 6 in proven data; FEAT_LSUI kept 8; x18 islands 10; "
      + "whole-function renames 12 by symbol, 14 by unwind entry", "every count is added and shown, svc_in_data too")

let off = await DebugNativeCounts.lineOffMain(libraries: folder)
check(off == line, "counted off the main thread it is the same line")

func apkconv_counts_of_single(_ file: URL) -> apkconv_counts {
    var found = apkconv_counts()
    var error = [CChar](repeating: 0, count: 256)
    precondition(apkconv_counts_of(file.path, &found, &error, error.count) == 0, "the converter counts the file")
    return found
}

func doubled(_ counts: apkconv_counts) -> apkconv_counts {
    var twice = counts
    twice.svc_rewritten *= 2
    twice.svc_data *= 2
    twice.svc_in_data *= 2
    twice.undefined_words *= 2
    twice.x18_islands *= 2
    twice.renames_by_symbol *= 2
    twice.renames_by_unwind *= 2
    return twice
}

exit(failures == 0 ? 0 : 1)
