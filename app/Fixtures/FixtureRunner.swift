#if DEBUG
import APKRuntime
import Darwin
import Foundation

/// Runs one test of fixtures/tests.txt (M2 step 6) when the app is launched with -runFixture <test>:
/// the program from the bundle's Fixtures folder (tools/embed-fixtures.sh), the way Android's shell
/// runs it: `cd /data/local/tmp && LD_LIBRARY_PATH=/data/local/tmp ./<program>`. With
/// -signOnDevice (M2b) the app converts and signs the program and its own libraries itself, from
/// the ELF files in Fixtures/elf, into Application Support, as it will do with the libraries of an
/// imported APK (ADR-0008, ADR-0022); output/signing then lists what that cost. Its stdout and
/// stderr go to tmp/fixtures/<test>/output/, where tools/device-suite.sh fetches them, and so does
/// its exit status: the program ends the process, and the runtime writes the status to output/status
/// first (devicectl's exit code is not reliable for a process that ends this quickly). So nothing
/// else may write to stdout or stderr, and the app never gets to its UI. output/traced says whether
/// a debugger was attached (1) or not (0): M2's criterion wants the suite to pass without one.
enum FixtureRunner {
    /// Where the exit status goes, also when the runner itself fails.
    nonisolated(unsafe) private static var statusDescriptor: Int32 = -1

    static func runIfRequested() {
        let arguments = ProcessInfo.processInfo.arguments
        guard let flag = arguments.firstIndex(of: "-runFixture"), arguments.indices.contains(flag + 1) else { return }
        run(test: arguments[flag + 1], signOnDevice: arguments.contains("-signOnDevice"))
    }

    private static func run(test: String, signOnDevice: Bool) -> Never {
        let folder = URL.temporaryDirectory.appending(path: "fixtures/\(test)", directoryHint: .isDirectory)
        let output = folder.appending(path: "output", directoryHint: .isDirectory)
        let dataLocalTmp = folder.appending(path: "data-local-tmp", directoryHint: .isDirectory)
        try? FileManager.default.removeItem(at: folder)
        do {
            try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: dataLocalTmp, withIntermediateDirectories: true)
        } catch {
            fail("cannot create \(folder.path): \(error)")
        }
        statusDescriptor = open(output.appending(path: "status").path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o644)
        apkrt_report_exit_status(statusDescriptor)
        redirect(STDOUT_FILENO, to: output.appending(path: "stdout"))
        redirect(STDERR_FILENO, to: output.appending(path: "stderr"))
        try? Data((isTraced() ? "1\n" : "0\n").utf8).write(to: output.appending(path: "traced"))

        guard let fixtures = Bundle.main.resourceURL?.appending(path: "Fixtures", directoryHint: .isDirectory),
              let fields = fields(of: test, in: fixtures.appending(path: "tests.txt"))
        else {
            fail("no fixture test \(test) in the app bundle; build the app with tools/device-suite.sh")
        }
        let program = fields.first { $0.hasPrefix("program=") }.map { String($0.dropFirst("program=".count)) } ?? test
        // The environment apk-run gives a program, as an Android shell does.
        var environment = [
            "PATH=/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin",
            "ANDROID_DATA=/data",
            "ANDROID_ROOT=/system",
            "HOME=/",
        ]
        if !fields.contains("no-library-path") {
            environment.append("LD_LIBRARY_PATH=/data/local/tmp")
        }

        var error = [CChar](repeating: 0, count: 1024)
        check(apkrt_mount("/data/local/tmp", dataLocalTmp.path, &error, error.count), error)
        check(apkrt_mount("/system", fixtures.appending(path: "system").path, &error, error.count), error)
        check(apkrt_add_library_directory("/system/lib64", fixtures.appending(path: "system-lib64").path, &error,
                                          error.count), error)
        check(apkrt_set_working_directory("/data/local/tmp", &error, error.count), error)

        // The program's libraries lie next to its container, in the program's directory.
        var containers = fixtures.appending(path: "tests/\(test)", directoryHint: .isDirectory)
        if signOnDevice {
            let libraries = fields.first { $0.hasPrefix("libraries=") }
                .map { $0.dropFirst("libraries=".count).split(separator: ",").map(String.init) } ?? []
            containers = convertAndSign([program] + libraries, of: test, from: fixtures, report: output.appending(path: "signing"))
        }
        let container = containers.appending(path: "\(program).dylib").path
        apkrt_run_program(container, cStrings(["./\(program)"]), cStrings(environment), &error, error.count)
        check(-1, error)
        fail("unreachable")
    }

    /// Converts and signs the ELF files `files` of the test into a fresh folder in Application
    /// Support and returns it. `report` gets one line per file: name, container bytes, milliseconds
    /// for converting and for signing.
    private static func convertAndSign(_ files: [String], of test: String, from fixtures: URL, report: URL) -> URL {
        let folder = URL.applicationSupportDirectory.appending(path: "fixtures/\(test)", directoryHint: .isDirectory)
        try? FileManager.default.removeItem(at: folder)
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            let signer = try CodeSigning.signer()
            var lines = ""
            for file in files {
                let elf = fixtures.appending(path: "elf/\(test)/\(file)")
                let statistics = try signer.convert(elf, to: folder.appending(path: "\(file).dylib"))
                lines += String(format: "%@ %llu %.3f %.3f\n", file, statistics.container_bytes,
                                statistics.convert_milliseconds, statistics.sign_milliseconds)
            }
            try Data(lines.utf8).write(to: report)
        } catch {
            fail("cannot convert and sign on the device: \(error.localizedDescription)")
        }
        return folder
    }

    /// The fields after the test's name on its line of tests.txt.
    private static func fields(of test: String, in list: URL) -> [String]? {
        guard let text = try? String(contentsOf: list, encoding: .utf8) else { return nil }
        for line in text.split(separator: "\n") where !line.hasPrefix("#") {
            let fields = line.split(separator: " ").map(String.init)
            if fields.first == test { return Array(fields.dropFirst()) }
        }
        return nil
    }

    /// Whether a debugger traces the process (P_TRACED), as Apple's Technical Q&A QA1361 checks it.
    private static func isTraced() -> Bool {
        var info = kinfo_proc()
        var size = MemoryLayout<kinfo_proc>.stride
        var name: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        guard sysctl(&name, UInt32(name.count), &info, &size, nil, 0) == 0 else { return false }
        return info.kp_proc.p_flag & P_TRACED != 0
    }

    private static func redirect(_ descriptor: Int32, to file: URL) {
        let opened = open(file.path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o644)
        guard opened >= 0, dup2(opened, descriptor) >= 0 else {
            fail("cannot write \(file.path): \(String(cString: strerror(errno)))")
        }
        close(opened)
    }

    /// NULL-terminated C strings for argv and envp. They stay allocated: the program ends the process.
    private static func cStrings(_ strings: [String]) -> [UnsafePointer<CChar>?] {
        strings.map { UnsafePointer(strdup($0)) } + [nil]
    }

    private static func check(_ result: Int32, _ error: [CChar]) {
        guard result != 0 else { return }
        fail(error.withUnsafeBufferPointer { String(cString: $0.baseAddress!) })
    }

    private static func fail(_ message: String) -> Never {
        let line = "FixtureRunner: \(message)\n"
        _ = line.withCString { write(STDERR_FILENO, $0, strlen($0)) }
        if statusDescriptor >= 0 {
            _ = "127\n".withCString { write(statusDescriptor, $0, strlen($0)) }
        }
        exit(127)
    }
}
#endif
