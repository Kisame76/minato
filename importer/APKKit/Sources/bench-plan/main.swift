import APKBench
import Foundation

// Tells tools/benchmark.sh what comes next in the parity series (ADR-0037) or test 2's (ADR-0042, --scene godot),
// from the run.csv files in <results>/<run>/: prints "run <run>" or "done"; on a stop it prints the reason to
// stderr and exits with 3.
// Usage: bench-plan next <results> [--scene parity|godot] [--api gles|vulkan|metal] [--more <configuration> ...]
func fail(_ message: String, status: Int32) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(status)
}

let usage = "usage: bench-plan next <results> [--scene parity|godot] [--api gles|vulkan|metal] "
    + "[--more <configuration> ...]"
var arguments = Array(CommandLine.arguments.dropFirst())
guard arguments.count >= 2, arguments.removeFirst() == "next" else { fail(usage, status: 2) }
let results = URL(filePath: arguments.removeFirst(), directoryHint: .isDirectory)
var scene = BenchScene.parity
var apis: [BenchAPI]?
var more: Set<BenchConfiguration> = []
while !arguments.isEmpty {
    let option = arguments.removeFirst()
    guard let value = arguments.first else { fail(usage, status: 2) }
    arguments.removeFirst()
    switch option {
    case "--scene":
        guard let named = BenchScene(rawValue: value) else { fail("unknown scene \(value)", status: 2) }
        scene = named
    case "--api":
        guard let api = BenchAPI(rawValue: value) else { fail("unknown API \(value)", status: 2) }
        apis = [api]
    case "--more":
        guard let configuration = BenchConfiguration(value) else { fail("unknown configuration \(value)", status: 2) }
        more.insert(configuration)
    default:
        fail(usage, status: 2)
    }
}
if let api = apis?.first, !scene.apis.contains(api) {
    fail("the \(scene.rawValue) scene has no \(api.rawValue) runs", status: 2)
}
if let other = more.first(where: { $0.scene != scene }) {
    fail("--more \(other) is not a \(scene.rawValue) configuration", status: 2)
}
let plan = BenchPlan(scene: scene, apis: apis, more: more)

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
