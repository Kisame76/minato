import APKBench
import Foundation

// Tells tools/benchmark.sh what comes next in the parity series (ADR-0037), from the run.csv files
// in <results>/<run>/: prints "run <run>" or "done"; on a stop it prints the reason to stderr and
// exits with 3.
// Usage: bench-plan next <results> [--api gles|vulkan] [--more <configuration> ...]
func fail(_ message: String, status: Int32) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(status)
}

let usage = "usage: bench-plan next <results> [--api gles|vulkan] [--more <configuration> ...]"
var arguments = Array(CommandLine.arguments.dropFirst())
guard arguments.count >= 2, arguments.removeFirst() == "next" else { fail(usage, status: 2) }
let results = URL(filePath: arguments.removeFirst(), directoryHint: .isDirectory)
var plan = BenchPlan()
while !arguments.isEmpty {
    let option = arguments.removeFirst()
    guard let value = arguments.first else { fail(usage, status: 2) }
    arguments.removeFirst()
    switch option {
    case "--api":
        guard let api = BenchAPI(rawValue: value) else { fail("unknown API \(value)", status: 2) }
        plan.apis = [api]
    case "--more":
        guard let configuration = BenchConfiguration(value) else { fail("unknown configuration \(value)", status: 2) }
        plan.more.insert(configuration)
    default:
        fail(usage, status: 2)
    }
}

var found: [BenchResult] = []
let runs = (try? FileManager.default.contentsOfDirectory(at: results, includingPropertiesForKeys: nil)) ?? []
for folder in runs where BenchRun(folder.lastPathComponent) != nil {
    let file = folder.appending(path: "run.csv")
    guard let text = try? String(contentsOf: file, encoding: .utf8) else { continue }
    do {
        found.append(try BenchCSV.result(fromRun: text))
    } catch {
        fail("\(file.path): \(error)", status: 3)
    }
}
switch plan.next(after: found) {
case .run(let run): print("run \(run)")
case .done: print("done")
case .stop(let reason): fail(reason, status: 3)
}
