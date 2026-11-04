import APKLibrary
import APKRuntime
import APKSession
import Darwin
import SwiftUI
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

/// Compile mode (ADR-0053, M6 c, E5): a process of its own, as ART allows one runtime per process, that runs dex2oat
/// for one app into its CompileWork folder in Caches and then restarts into the library, which signs and keeps the
/// code. No other guest runtime starts in it. The runtime writes dex2oat's exit status to the folder, also at a crash;
/// a process that iOS ends, for using too much memory say, leaves none, and the library records a failure for it.
@MainActor
@Observable
final class CompileSession {
    let entry: LibraryEntry
    /// The restart did not work: the user opens Minato again.
    private(set) var restartFailed = false
    @ObservationIgnored private var work: CompileWork?
    @ObservationIgnored private var started = false
    /// The process's session, for the end handler, which cannot capture.
    private static var current: CompileSession?

    init(entry: LibraryEntry) {
        self.entry = entry
    }

    /// Once the view shows: prepares the folder and starts dex2oat on a thread of its own; back to the library when it
    /// cannot start.
    func start() {
        guard !started else { return }
        started = true
        Self.current = self
        guard let paths = try? LibraryPaths.standard(), let bundle = Bundle.main.resourceURL,
              let digest = entry.base?.sha256 else { return back("the app's files are not known") }
        let work = CompileWork(caches: paths.caches, package: entry.id)
        self.work = work
        let apk = paths.versionDirectory(for: entry).appending(path: "base.apk").path
        guard let start = GuestStart(compile: entry.id, apk: apk, apkSHA256: digest, output: work.folder.path,
                                     guestSystem: bundle.appending(path: "Guest").path)
        else { return back("the app's digest is no SHA-256") }
        do {
            try work.prepare()
        } catch {
            return back("its folder: \(error.localizedDescription)")
        }
        let descriptor = open(work.statusFile.path, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
        apkrt_report_exit_status(descriptor)
        if let failure = Self.begin(start) { return back(failure) }
        #if DEBUG
        SessionCheck.guestStarts()
        #endif
        log.info("Compiling \(self.entry.id, privacy: .public)")
        #if DEBUG
        SessionCheck.event("compile started \(entry.id)")
        #endif
        Task.detached {
            if let failure = Self.startProgram(start) {
                await MainActor.run { self.back(failure) }
            }
        }
    }

    /// Cancel: nothing is recorded, so the app is compiled after a later session.
    func cancel() {
        try? work?.cancel()
        restart()
    }

    /// dex2oat ended, through the end handler: its status is in the folder already.
    private func ended(status: Int32) {
        log.info("Compiling \(self.entry.id, privacy: .public) ended with status \(status)")
        #if DEBUG
        SessionCheck.event("compile ended \(status) peak \(Self.peakFootprintMegabytes)")
        #endif
        restart()
    }

    /// It did not start: the folder has no status, which the library takes as a failure.
    private func back(_ reason: String) {
        log.error("\(self.entry.id, privacy: .public) is not compiled: \(reason, privacy: .public)")
        #if DEBUG
        SessionCheck.event("compile not started \(reason)")
        #endif
        restart()
    }

    #if DEBUG
    /// The process's peak physical footprint in MB, the memory iOS counts against it (the compile measures), -1 when
    /// the kernel does not say.
    private static var peakFootprintMegabytes: Int {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
        let result = withUnsafeMutablePointer(to: &info) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return result == KERN_SUCCESS ? Int(info.ledger_phys_footprint_peak / 1_048_576) : -1
    }
    #endif

    private func restart() {
        Restart.now { [weak self] in self?.restartFailed = true }
    }

    /// The compile start's paths and a session with only an end handler, so that dex2oat's end comes back here
    /// instead of ending the process; no activity-host connects. Nil, or why it cannot start.
    private static func begin(_ start: GuestStart) -> String? {
        var error = [CChar](repeating: 0, count: 1024)
        for path in start.mounts where apkrt_mount(path.guest, path.host, &error, error.count) != 0 {
            return text(error)
        }
        for path in start.libraryDirectories
        where apkrt_add_library_directory(path.guest, path.host, &error, error.count) != 0 {
            return text(error)
        }
        var session = apkrt_session()
        session.end = endHandler
        guard apkrt_session_begin(&session) == 0 else { return "the session did not begin" }
        return nil
    }

    /// dex2oat64, not on the main thread, where it would map the program. Nil, or why it cannot start.
    private nonisolated static func startProgram(_ start: GuestStart) -> String? {
        var error = [CChar](repeating: 0, count: 1024)
        let copies = (start.arguments.map { strdup($0) }, start.environment.map { strdup($0) })
        defer { (copies.0 + copies.1).forEach { free($0) } }
        let arguments = copies.0.map { $0.map { UnsafePointer($0) } } + [nil]
        let environment = copies.1.map { $0.map { UnsafePointer($0) } } + [nil]
        let result = arguments.withUnsafeBufferPointer { argv in
            environment.withUnsafeBufferPointer { envp in
                apkrt_start_program(start.program, argv.baseAddress!, envp.baseAddress!, &error, error.count)
            }
        }
        return result == 0 ? nil : text(error)
    }

    private static let endHandler: apkrt_end_handler = { _, status in
        DispatchQueue.main.async {
            MainActor.assumeIsolated { CompileSession.current?.ended(status: status) }
        }
    }

    private nonisolated static func text(_ error: [CChar]) -> String {
        error.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
    }
}

/// Compile mode's screen: the app, "Optimizing", and Cancel, with Apple's own pieces.
struct CompileModeView: View {
    @State private var session: CompileSession
    private let iconURL: URL?

    init(entry: LibraryEntry) {
        _session = State(initialValue: CompileSession(entry: entry))
        iconURL = entry.iconFile.flatMap { file in
            (try? LibraryPaths.standard()).map { $0.applicationSupport.appending(path: file) }
        }
    }

    var body: some View {
        VStack(spacing: 20) {
            AppIcon(url: iconURL)
                .frame(width: 96, height: 96)
            VStack(spacing: 8) {
                Text("Optimizing “\(session.entry.displayName)”")
                    .font(.headline)
                    .multilineTextAlignment(.center)
                Text("So that it runs faster from now on. This happens once for each version.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            ProgressView()
            Button("Cancel") { session.cancel() }
        }
        .padding(32)
        .onAppear { session.start() }
        // The restart did not work (ADR-0032, decision 2): Minato opens the library at its next launch.
        .alert("Open Minato again.", isPresented: .constant(session.restartFailed)) {
            Button("OK") { exit(0) }
        }
    }
}
