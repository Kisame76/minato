import APKLibrary
import APKRuntime
import APKSession
import Darwin

/// Game mode's part of ADR-0032's decision 8: at its start it writes the last session's record, and hands
/// the runtime the status file for the exit status, which the runtime writes also at a crash; when the app
/// cannot start, it adds the reason. The library shows an end that went wrong at its next start.
@MainActor
enum SessionEndReport {
    private static var store: LastSessionStore?
    /// The status file's descriptor, open for the process's life; -1 without one.
    private static var descriptor: Int32 = -1

    /// Before the program starts.
    static func begin(_ entry: LibraryEntry) {
        guard let paths = try? LibraryPaths.standard() else { return }
        let store = LastSessionStore(folder: paths.applicationSupport)
        descriptor = store.begin(LastSession(package: entry.id, name: entry.displayName))
        self.store = store
        apkrt_report_exit_status(descriptor)
    }

    /// START_FAILED, or `apkrt_start_program`'s error.
    static func startFailed(_ reason: String) {
        store?.failed(reason)
    }

    #if DEBUG
    /// For session-check: the status as the runtime reports it at a program's end, then the process ends.
    static func endAsTheRuntimeWould(_ status: Int32) {
        let text = "\(status)\n"
        _ = text.withCString { write(descriptor, $0, strlen($0)) }
        _exit(1)
    }
    #endif
}
