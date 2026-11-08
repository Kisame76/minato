#if DEBUG
import APKRuntime
import Darwin
import Foundation
import UIKit

/// Runs one test of fixtures/tests.txt (M2 step 6), or on the iPhone of fixtures/iphone-only.txt, which a Simulator
/// build refuses, when the app is launched with -runFixture <test>:
/// the program from the bundle's Fixtures folder (tools/embed-fixtures.sh), the way Android's shell
/// runs it: `cd /data/local/tmp && LD_LIBRARY_PATH=/data/local/tmp ./<program>`. With
/// -signOnDevice (M2b) the app converts and signs the program and its own libraries itself, from
/// the ELF files in Fixtures/elf, into Application Support, as it will do with the libraries of an
/// imported APK (ADR-0008, ADR-0022); output/signing then lists what that cost. Its stdout and
/// stderr go to tmp/fixtures/<test>/output/, where tools/device-suite.sh fetches them, and so does
/// its exit status: the program ends the process, and the runtime writes the status to output/status
/// first (devicectl's exit code is not reliable for a process that ends this quickly). So nothing
/// else may write to stdout or stderr: the app shows an empty scene. output/traced says whether
/// a debugger was attached (1) or not (0): M2's criterion wants the suite to pass without one. After
/// the status the runtime writes what the run measured to output/measures (ADR-0033 d1), and while it runs the
/// memory watermark to output/memory (apkrt_report_memory_to, ADR-0032 d8). With
/// -frameRateUnlock 60|120 the program runs with the FPS unlock (ADR-0066 step 4, apkrt_set_frame_rate_unlock).
///
/// The program starts once the app's launch has finished, at its first didBecomeActive, on a thread
/// of its own (apkrt_start_program): iOS's launch watchdog ends an app that has not finished
/// launching after 20 s, which ended ART's longer run-tests on the iPhone (ADR-0033 d3).
/// output/launch says the app's state as the program starts, "active" (tools/fixture-launch-check.sh).
///
/// With -runART <test> [-flavor debug] it runs one of ART's tests instead (ADR-0033 d2), as
/// runtime/tests/device_art.py stages them and runs them on the Mac: dalvikvm64 with the command
/// of Fixtures/art/tests/<test>/command.json (command-debug.json in the debug flavor), the guest's
/// /apex and /system from the bundle's Guest folder (tools/embed-guest-system.sh), and a new /data.
enum FixtureRunner {
    /// Where the exit status goes, also when the runner itself fails.
    nonisolated(unsafe) private static var statusDescriptor: Int32 = -1
    /// Whether the app has become active, which ends its launch, and the test started.
    nonisolated(unsafe) private static var launched = false

    /// The environment apk-run gives a program, as an Android shell does.
    private static let shellEnvironment = [
        "PATH=/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin",
        "ANDROID_DATA=/data",
        "ANDROID_ROOT=/system",
        "HOME=/",
    ]

    /// Whether the app was launched for a test: then its program starts at the app's first
    /// didBecomeActive, and the app touches nothing else.
    static func startAfterLaunchIfRequested() -> Bool {
        let arguments = ProcessInfo.processInfo.arguments
        let start: @Sendable () -> Void
        if let flag = arguments.firstIndex(of: "-runART"), arguments.indices.contains(flag + 1) {
            let test = arguments[flag + 1]
            let debug = arguments.firstIndex(of: "-flavor").map { arguments.dropFirst($0 + 1).first ?? "" } == "debug"
            start = { runART(test: test, debug: debug) }
        } else if let flag = arguments.firstIndex(of: "-runFixture"), arguments.indices.contains(flag + 1) {
            let test = arguments[flag + 1]
            let signOnDevice = arguments.contains("-signOnDevice")
            let unlock = arguments.firstIndex(of: "-frameRateUnlock").map { arguments.dropFirst($0 + 1).first ?? "" }
            start = { run(test: test, signOnDevice: signOnDevice, frameRateUnlock: unlock) }
        } else {
            return false
        }
        // The runtime's fault line (ADR-0032 decision 8) is on in a Debug build, which this one is; off for these
        // byte-compared runs, as tools/run-fixture.cmake has it, or signal-test's and c-signal-test's stderr would
        // differ from the expected. The runtime reads it once, at the first program's start.
        setenv("MINATO_FAULT_LOG", "0", 1)
        NotificationCenter.default.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil,
                                               queue: .main) { _ in
            guard !launched else { return }
            launched = true
            start()
        }
        return true
    }

    private static func run(test: String, signOnDevice: Bool, frameRateUnlock: String?) {
        let folder = prepare(test: test)
        let dataLocalTmp = folder.appending(path: "data-local-tmp", directoryHint: .isDirectory)
        do {
            try FileManager.default.createDirectory(at: dataLocalTmp, withIntermediateDirectories: true)
        } catch {
            fail("cannot create \(dataLocalTmp.path): \(error)")
        }
        let output = folder.appending(path: "output", directoryHint: .isDirectory)

        guard let fixtures = Bundle.main.resourceURL?.appending(path: "Fixtures", directoryHint: .isDirectory) else {
            fail("no fixture test \(test) in the app bundle; build the app with tools/device-suite.sh")
        }
        let iPhoneOnly = fields(of: test, in: fixtures.appending(path: "iphone-only.txt"))
        #if targetEnvironment(simulator)
        if iPhoneOnly != nil {
            fail("\(test) runs on the iPhone only (fixtures/iphone-only.txt): a Simulator app runs on the Mac's memory")
        }
        #endif
        guard let fields = fields(of: test, in: fixtures.appending(path: "tests.txt")) ?? iPhoneOnly else {
            fail("no fixture test \(test) in the app bundle; build the app with tools/device-suite.sh")
        }
        let program = fields.first { $0.hasPrefix("program=") }.map { String($0.dropFirst("program=".count)) } ?? test
        var environment = shellEnvironment
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
        if let frameRateUnlock {
            guard let rate = Int32(frameRateUnlock), apkrt_set_frame_rate_unlock(rate) == 0 else {
                fail("the FPS unlock takes 60 or 120, not \(frameRateUnlock)")
            }
        }
        check(apkrt_start_program(container, cStrings(["./\(program)"]), cStrings(environment), &error, error.count),
              error)
    }

    /// How tools/device-suite.sh staged an ART test (runtime/tests/device_art.py; ADR-0033 d2's contract).
    private struct ARTCommand: Decodable {
        /// argv; argv[0] is /apex/com.android.art/bin/dalvikvm64.
        let arguments: [String]
        /// "NAME=value" entries, each replacing the shell's entry of its name.
        let environment: [String]
        let workingDirectory: String
        /// Guest folders under /data, made empty.
        let folders: [String]
        /// The guest folder under /data that gets the test's files, read-only.
        let files: String
        /// The guest folder whose libraries are the test's JNI containers, if it has any.
        let libraries: String?
    }

    private static func runART(test: String, debug: Bool) {
        let folder = prepare(test: test)
        guard let resources = Bundle.main.resourceURL else { fail("no bundle resources") }
        let art = resources.appending(path: "Fixtures/art", directoryHint: .isDirectory)
        let guest = resources.appending(path: "Guest", directoryHint: .isDirectory)
        let staged = art.appending(path: "tests/\(test)", directoryHint: .isDirectory)
        let commandFile = staged.appending(path: debug ? "command-debug.json" : "command.json")
        guard let json = try? Data(contentsOf: commandFile) else {
            fail("no ART test \(test) in the app bundle; build the app with tools/device-suite.sh")
        }
        let command: ARTCommand
        do {
            command = try JSONDecoder().decode(ARTCommand.self, from: json)
        } catch {
            fail("cannot read \(commandFile.lastPathComponent) of \(test): \(error)")
        }

        // A new /data: dalvik-cache and /data/local/tmp as every run has them, the command's folders, and its files,
        // read-only, since ART refuses an app's dex file that the app may write (SDK 34 on).
        let data = folder.appending(path: "data", directoryHint: .isDirectory)
        func host(_ guestPath: String) -> URL {
            guard guestPath == "/data" || guestPath.hasPrefix("/data/") else {
                fail("\(test): \(guestPath) is not under /data")
            }
            return data.appending(path: String(guestPath.dropFirst("/data".count)), directoryHint: .isDirectory)
        }
        do {
            for guestPath in ["/data/dalvik-cache/arm64", "/data/local/tmp"] + command.folders + [command.files] {
                try FileManager.default.createDirectory(at: host(guestPath), withIntermediateDirectories: true)
            }
            let files = staged.appending(path: "files", directoryHint: .isDirectory)
            for name in try FileManager.default.contentsOfDirectory(atPath: files.path).sorted() {
                let target = host(command.files).appending(path: name)
                try FileManager.default.copyItem(at: files.appending(path: name), to: target)
                try makeReadOnly(target)
            }
        } catch {
            fail("cannot set up \(test)'s /data: \(error)")
        }

        var error = [CChar](repeating: 0, count: 1024)
        check(apkrt_mount("/data", data.path, &error, error.count), error)
        check(apkrt_mount("/apex", guest.appending(path: "apex").path, &error, error.count), error)
        check(apkrt_mount("/system", guest.appending(path: "system").path, &error, error.count), error)
        check(apkrt_add_library_directory("/system/lib64", guest.appending(path: "lib64").path, &error, error.count),
              error)
        // The runtime takes a library from the first folder that has it: the debug flavor's six come first.
        if debug {
            check(apkrt_add_library_directory("/apex/com.android.art/lib64", art.appending(path: "lib64-debug").path,
                                              &error, error.count), error)
        }
        check(apkrt_add_library_directory("/apex/com.android.art/lib64",
                                          guest.appending(path: "apex-lib64/com.android.art").path, &error,
                                          error.count), error)
        check(apkrt_add_library_directory("/apex/com.android.conscrypt/lib64",
                                          guest.appending(path: "apex-lib64/com.android.conscrypt").path, &error,
                                          error.count), error)
        if let libraries = command.libraries {
            check(apkrt_add_library_directory(libraries, staged.appending(path: "libraries").path, &error,
                                              error.count), error)
        }
        check(apkrt_set_working_directory(command.workingDirectory, &error, error.count), error)

        // The command's entries replace the shell's of the same name: the runtime keeps a name's first entry.
        var environment = shellEnvironment
        for entry in command.environment {
            let name = entry.prefix { $0 != "=" }
            if let index = environment.firstIndex(where: { existing in existing.prefix { $0 != "=" } == name }) {
                environment[index] = entry
            } else {
                environment.append(entry)
            }
        }
        let dalvikvm = art.appending(path: "bin/dalvikvm64.dylib").path
        check(apkrt_start_program(dalvikvm, cStrings(command.arguments), cStrings(environment), &error, error.count),
              error)
    }

    /// A new tmp/fixtures/<test>/ with its output folder, which gets the status, stdout, stderr, traced, the
    /// measures and the memory watermark; returns the test's folder.
    private static func prepare(test: String) -> URL {
        let folder = URL.temporaryDirectory.appending(path: "fixtures/\(test)", directoryHint: .isDirectory)
        let output = folder.appending(path: "output", directoryHint: .isDirectory)
        try? FileManager.default.removeItem(at: folder)
        do {
            try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        } catch {
            fail("cannot create \(folder.path): \(error)")
        }
        statusDescriptor = open(output.appending(path: "status").path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o644)
        apkrt_report_exit_status(statusDescriptor)
        let measures = open(output.appending(path: "measures").path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o644)
        apkrt_report_exit_measures(measures)
        // The watermark's fixed block, written over again while the program runs, also when jetsam ends it.
        let memory = open(output.appending(path: "memory").path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o644)
        apkrt_report_memory_to(memory)
        redirect(STDOUT_FILENO, to: output.appending(path: "stdout"))
        redirect(STDERR_FILENO, to: output.appending(path: "stderr"))
        try? Data((isTraced() ? "1\n" : "0\n").utf8).write(to: output.appending(path: "traced"))
        // The app's state as the program starts: "active" once its launch has finished.
        try? Data((launched ? "active\n" : "launching\n").utf8).write(to: output.appending(path: "launch"))
        return folder
    }

    /// Regular files read-only (0444), folders inside left as they are, so that they stay open.
    private static func makeReadOnly(_ item: URL) throws {
        var isFolder: ObjCBool = false
        guard FileManager.default.fileExists(atPath: item.path, isDirectory: &isFolder) else { return }
        if isFolder.boolValue {
            for name in try FileManager.default.contentsOfDirectory(atPath: item.path) {
                try makeReadOnly(item.appending(path: name))
            }
        } else {
            try FileManager.default.setAttributes([.posixPermissions: 0o444], ofItemAtPath: item.path)
        }
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
