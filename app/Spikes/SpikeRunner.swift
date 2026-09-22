#if DEBUG
import Darwin
import Foundation

/// Runs the feasibility spikes S1–S3 and S1b (docs/STATUS.md) when the app is launched with -runSpikes,
/// prints a report to stdout and exits before any UI appears. `tools/spikes.sh` starts it on the
/// iPhone and shows the output.
enum SpikeRunner {
    static func runIfRequested() {
        guard ProcessInfo.processInfo.arguments.contains("-runSpikes") else { return }
        report("SPIKES on \(machine()), iOS \(ProcessInfo.processInfo.operatingSystemVersionString), "
            + "started \(Date.now.formatted(.iso8601))")
        // S1b first: none of its copies may meet an image that S1 already loaded from the bundle.
        runS1b()
        runS1()
        runS2()
        runS3()
        report("SPIKES done")
        exit(0)
    }

    // MARK: - S1

    /// Can the app load a dylib that was signed on the Mac with the Personal Team certificate,
    /// from its data container instead of the bundle?
    private static func runS1() {
        report("S1 dylib signed on the Mac, loaded from Application Support")
        guard let signed = Bundle.main.url(forResource: "SpikeS1", withExtension: "dylib", subdirectory: "Spikes"),
              let adHoc = Bundle.main.url(forResource: "SpikeS1-adhoc", withExtension: "dylib", subdirectory: "Spikes")
        else {
            report("  test dylibs missing in the app bundle")
            return
        }
        do {
            let folder = URL.applicationSupportDirectory.appending(path: "spikes", directoryHint: .isDirectory)
            try? FileManager.default.removeItem(at: folder)
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            // Written byte by byte, like the converter in M2 will write its output (no clone).
            let copied = folder.appending(path: "SpikeS1.dylib")
            let copiedAdHoc = folder.appending(path: "SpikeS1-adhoc.dylib")
            try Data(contentsOf: signed).write(to: copied)
            try Data(contentsOf: adHoc).write(to: copiedAdHoc)

            report("  team-signed copy:     \(load(copied))")
            report("  team-signed in app:   \(load(signed))")
            // Last, in case the kernel kills the process instead of failing dlopen.
            report("  ad-hoc copy (control, must fail): \(load(copiedAdHoc))")
        } catch {
            report("  setup failed: \(error)")
        }
    }

    // MARK: - S1b

    /// Control test for S1 (ADR-0008): the same dylib, but with the app's bundle ID as signing identifier,
    /// as LiveContainer signs guest code, copied to tmp/ and to Application Support. The S1 variant in tmp/
    /// separates the identifier from the location.
    private static func runS1b() {
        report("S1b dylib signed on the Mac with the bundle ID as identifier, loaded from tmp/ and Application Support")
        guard let bundleID = Bundle.main.url(forResource: "SpikeS1-bundleid", withExtension: "dylib", subdirectory: "Spikes"),
              let spikeS1 = Bundle.main.url(forResource: "SpikeS1", withExtension: "dylib", subdirectory: "Spikes")
        else {
            report("  test dylibs missing in the app bundle")
            return
        }
        do {
            let tmp = try freshFolder(URL.temporaryDirectory.appending(path: "spikes-s1b", directoryHint: .isDirectory))
            let support = try freshFolder(
                URL.applicationSupportDirectory.appending(path: "spikes-s1b", directoryHint: .isDirectory))
            let copies = [
                ("bundle-ID copy in tmp/", bundleID, tmp.appending(path: "SpikeS1-bundleid.dylib")),
                ("bundle-ID copy in Application Support", bundleID, support.appending(path: "SpikeS1-bundleid.dylib")),
                ("SpikeS1 copy in tmp/ (control)", spikeS1, tmp.appending(path: "SpikeS1.dylib")),
            ]
            for (label, source, copy) in copies {
                // Written byte by byte like in S1, so each copy is a new file the kernel has not seen.
                try Data(contentsOf: source).write(to: copy)
                report("  \(label):")
                report("    \(checkSignature(copy))")
                report("    \(load(copy))")
            }
            report("  bundle-ID original in the app (control, must load):")
            report("    \(checkSignature(bundleID))")
            report("    \(load(bundleID))")
        } catch {
            report("  setup failed: \(error)")
        }
    }

    private static func freshFolder(_ folder: URL) throws -> URL {
        try? FileManager.default.removeItem(at: folder)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder
    }

    private static func checkSignature(_ url: URL) -> String {
        let check = spike_s1b_check_signature(url.path)
        guard check.open_errno == 0 else { return "cannot read the signature: \(errorName(check.open_errno))" }
        let message = withUnsafeBytes(of: check.library_validation_message) { bytes in
            String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
        }
        let signatures = check.add_signatures == -1 ? "failed, \(errorName(check.add_signatures_errno))" : "ok"
        let validation = check.library_validation == -1
            ? "failed, \(errorName(check.library_validation_errno))" + (message.isEmpty ? "" : ", \"\(message)\"")
            : "ok"
        return "F_ADDFILESIGS_RETURN \(signatures); F_CHECK_LV \(validation)"
    }

    private static func errorName(_ code: Int32) -> String {
        "\(String(cString: strerror(code))) (errno=\(code))"
    }

    private static func load(_ url: URL) -> String {
        let start = ContinuousClock.now
        guard let handle = dlopen(url.path, RTLD_NOW | RTLD_LOCAL) else {
            return "dlopen failed: " + (dlerror().map { String(cString: $0) } ?? "no reason given")
        }
        let elapsed = ContinuousClock.now - start
        guard let answer = dlsym(handle, "spike_answer"), let counter = dlsym(handle, "spike_counter") else {
            return "loaded, but the symbols are missing"
        }
        typealias Function = @convention(c) () -> Int32
        let next = unsafeBitCast(counter, to: Function.self)
        let counts = [next(), next()]
        var info = Dl_info()
        let image = dladdr(answer, &info) != 0 ? info.dli_fname.map { String(cString: $0) } ?? "?" : "?"
        return "OK, spike_answer() = \(unsafeBitCast(answer, to: Function.self)()), spike_counter() = \(counts), "
            + "dlopen \(elapsed.formatted(.units(allowed: [.milliseconds], fractionalPart: .show(length: 2)))), "
            + "image …/\(URL(fileURLWithPath: image).pathComponents.suffix(2).joined(separator: "/"))"
    }

    // MARK: - S2

    /// How much memory and address space does the app get with the free account?
    private static func runS2() {
        report("S2 memory and address space")
        let memory = spike_s2_memory()
        report("  physical RAM \(gib(memory.physical)), page size \(memory.page_size / 1024) KiB")
        report("  os_proc_available_memory \(gib(memory.available)), footprint now \(mib(memory.footprint))")
        report("  jetsam limit ≈ footprint + available = \(gib(memory.footprint + memory.available)), "
            + "limit_bytes_remaining \(gib(memory.limit_remaining))")
        let space = spike_s2_address_space(16 << 20)
        report("  largest PROT_NONE reservation \(gib(space.largest))")
        report("  all reservations \(gib(space.total)) in \(space.regions) regions, highest end 0x\(hex(space.highest_end))")
    }

    // MARK: - S3

    /// Does iOS 27 behave like XNU 12377: TPIDR_EL0 overwritten, x18 zeroed?
    private static func runS3() {
        report("S3 TPIDR_EL0 and x18 (3 runs each, fresh thread per run)")
        report("  TPIDRRO_EL0 (TSD base) 0x\(hex(spike_s3_tpidrro_el0()))")
        for (name, probe) in [("TPIDR_EL0", spike_s3_tpidr_el0), ("x18", spike_s3_x18)] {
            let runs = (0..<3).map { _ in probe() }
            report("  \(name): before writing \(runs.map { "0x" + hex($0.initial) }.joined(separator: ", ")), wrote 0x\(hex(runs[0].written))")
            report("    read back at once        \(outcome(runs.map(\.immediately), runs))")
            report("    after syscall (svc)      \(outcome(runs.map(\.after_syscall), runs))")
            report("    after usleep (blocked)   \(outcome(runs.map(\.after_sleep), runs))")
            report("    after 200 ms spin        \(outcome(runs.map(\.after_spin), runs))")
        }
    }

    private static func outcome(_ values: [UInt64], _ runs: [spike_register_trace]) -> String {
        zip(values, runs).map { value, run in value == run.written ? "kept" : "changed→0x\(hex(value))" }
            .joined(separator: ", ")
    }

    // MARK: - Output

    /// Unbuffered, so every line reaches devicectl even if the process dies right after it.
    private static func report(_ line: String) {
        FileHandle.standardOutput.write(Data((line + "\n").utf8))
    }

    private static func machine() -> String {
        var size = 0
        sysctlbyname("hw.machine", nil, &size, nil, 0)
        var bytes = [CChar](repeating: 0, count: size)
        sysctlbyname("hw.machine", &bytes, &size, nil, 0)
        return bytes.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
    }

    private static func gib(_ bytes: UInt64) -> String {
        String(format: "%.2f GiB", Double(bytes) / 1_073_741_824)
    }

    private static func mib(_ bytes: UInt64) -> String {
        String(format: "%.0f MiB", Double(bytes) / 1_048_576)
    }

    private static func hex(_ value: UInt64) -> String {
        String(value, radix: 16)
    }
}
#endif
