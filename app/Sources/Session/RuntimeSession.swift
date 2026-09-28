import APKRuntime
import APKSession
import Foundation
import QuartzCore

/// ADR-0028's session and window registry (`apkrt.h`) for the bridge: the events go to activity-host,
/// and the window is the view's CAMetalLayer.
final class RuntimeSessionHost: SessionHost {
    /// The layer every window of the session is on, once the view exists; the registry retains it while a
    /// window is there. The bridge adds windows only after the first layout.
    var layer: CAMetalLayer?

    func post(_ event: SessionEvent) -> UInt32 {
        let (type, a0, a1, a2) = Self.record(event)
        return apkrt_session_post(type.rawValue, a0, a1, a2)
    }

    func addWindow(width: Int32, height: Int32) -> UInt64 {
        apkrt_window_add(layer.map { Unmanaged.passUnretained($0).toOpaque() }, width, height)
    }

    func setGate(_ window: UInt64, open: Bool) {
        _ = apkrt_window_set_gate(window, open ? 1 : 0)
    }

    func removeWindow(_ window: UInt64) {
        _ = apkrt_window_remove(window)
    }

    func setBackground(_ on: Bool) {
        apkrt_set_background(on ? 1 : 0)
    }

    func waitScheduled(seconds: Double) -> Bool {
        apkrt_wait_scheduled(seconds) == 1
    }

    /// `apkrt_session_wait`, never on the main thread: 1 answered, 0 timed out, -1 the channel broke or the
    /// program ended.
    static func wait(serial: UInt32, seconds: Double) -> Int32 {
        apkrt_session_wait(serial, seconds)
    }

    /// The frames presented through the window's gate so far, for the FPS label; 0 once it is removed.
    static func presents(_ window: UInt64) -> UInt64 {
        apkrt_window_presents(window)
    }

    /// The event's type and arguments (`guest_interface.h`).
    static func record(_ event: SessionEvent) -> (apkrt_event_type, UInt64, UInt64, UInt64) {
        switch event {
        case .create: (APKRT_EVENT_CREATE, 0, 0, 0)
        case .start: (APKRT_EVENT_START, 0, 0, 0)
        case .resume: (APKRT_EVENT_RESUME, 0, 0, 0)
        case .pause: (APKRT_EVENT_PAUSE, 0, 0, 0)
        case .stop: (APKRT_EVENT_STOP, 0, 0, 0)
        case .destroy: (APKRT_EVENT_DESTROY, 0, 0, 0)
        case .saveInstanceState: (APKRT_EVENT_SAVE_INSTANCE_STATE, 0, 0, 0)
        case .windowFocusChanged(let focus): (APKRT_EVENT_WINDOW_FOCUS_CHANGED, focus ? 1 : 0, 0, 0)
        case .nativeWindowCreated(let window, let width, let height):
            (APKRT_EVENT_NATIVE_WINDOW_CREATED, window, UInt64(width), UInt64(height))
        case .nativeWindowDestroyed(let window): (APKRT_EVENT_NATIVE_WINDOW_DESTROYED, window, 0, 0)
        case .lowMemory: (APKRT_EVENT_LOW_MEMORY, 0, 0, 0)
        case .inputQueueCreated(let channel): (APKRT_EVENT_INPUT_QUEUE_CREATED, channel, 0, 0)
        case .nativeWindowResized(let window, let width, let height):
            (APKRT_EVENT_NATIVE_WINDOW_RESIZED, window, UInt64(width), UInt64(height))
        case .contentRectChanged(let left, let top, let right, let bottom):
            (APKRT_EVENT_CONTENT_RECT_CHANGED, pair(left, top), pair(right, bottom), 0)
        case .nativeWindowRedrawNeeded(let window): (APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED, window, 0, 0)
        }
    }

    /// `low | high << 32`, 32 bits each (`guest_interface.h`).
    private static func pair(_ low: Int32, _ high: Int32) -> UInt64 {
        UInt64(UInt32(bitPattern: low)) | UInt64(UInt32(bitPattern: high)) << 32
    }

    /// The paths of decision 14 and the session with its configuration and handlers, before activity-host
    /// starts (ADR-0028 d). Nil, or why the app cannot start.
    static func begin(_ start: GuestStart, configuration: GuestConfiguration) -> String? {
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
        session.request = requestHandler
        session.configuration = record(configuration)
        guard apkrt_session_begin(&session) == 0 else { return "the session did not begin" }
        return nil
    }

    /// activity-host, once the session has begun; not on the main thread, where it would map the program.
    /// Nil, or why it cannot start.
    static func startProgram(_ start: GuestStart) -> String? {
        var error = [CChar](repeating: 0, count: 1024)
        let result = withCStrings(start.arguments) { arguments in
            withCStrings(start.environment) { environment in
                apkrt_start_program(start.program, arguments, environment, &error, error.count)
            }
        }
        return result == 0 ? nil : text(error)
    }

    static func record(_ configuration: GuestConfiguration) -> apkrt_configuration {
        var record = apkrt_configuration()
        record.scale = configuration.scale
        record.width = configuration.width
        record.height = configuration.height
        record.orientation = configuration.orientation
        record.night = configuration.night
        record.refresh_rate = configuration.refreshRate
        // Three letters at most and a NUL (GuestConfiguration keeps only codes that fit).
        withUnsafeMutableBytes(of: &record.language) { $0.copyBytes(from: configuration.language.utf8.prefix(3)) }
        withUnsafeMutableBytes(of: &record.country) { $0.copyBytes(from: configuration.country.utf8.prefix(3)) }
        return record
    }

    private static func text(_ error: [CChar]) -> String {
        error.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
    }

    /// NULL-terminated C strings for the duration of `body`.
    private static func withCStrings<Result>(
        _ strings: [String], _ body: (UnsafePointer<UnsafePointer<CChar>?>) -> Result
    ) -> Result {
        let copies = strings.map { strdup($0) }
        defer { copies.forEach { free($0) } }
        let pointers = copies.map { $0.map { UnsafePointer($0) } } + [nil]
        return pointers.withUnsafeBufferPointer { body($0.baseAddress!) }
    }

    /// The guest's request as the runtime reports it (`apkrt_request_handler`), or nil for a type it does
    /// not report to the host.
    static func request(type: Int32, argument: UInt64, text: UnsafePointer<CChar>?) -> SessionRequest? {
        switch UInt32(bitPattern: type) {
        case APKRT_REQUEST_FINISH.rawValue: .finish
        case APKRT_REQUEST_KEEP_SCREEN_ON.rawValue: .keepScreenOn(argument != 0)
        case APKRT_REQUEST_START_FAILED.rawValue: .startFailed(text.map { String(cString: $0) } ?? "")
        default: nil
        }
    }

    /// The session's end handler: like the request handler it runs on a guest thread, with a stack that may
    /// be small, so the main queue takes it over; the thread then sleeps for good (`apkrt.h`).
    static let endHandler: apkrt_end_handler = { _, status in
        DispatchQueue.main.async {
            MainActor.assumeIsolated { GameSession.current?.programEnded(status: status) }
        }
    }

    /// The session's request handler: it runs on the guest thread that asks and must not block, so the
    /// request goes to the main queue; the text lives only until it returns, so it is copied first.
    static let requestHandler: apkrt_request_handler = { _, type, argument, text in
        guard let request = RuntimeSessionHost.request(type: type, argument: argument, text: text) else { return }
        DispatchQueue.main.async {
            MainActor.assumeIsolated { GameSession.current?.received(request) }
        }
    }
}

#if DEBUG
/// A session without a guest, for session-check's rounds of game mode itself (the request "no-guest"): every
/// post goes through, windows get IDs of their own, and every wait is answered at once.
final class NoGuestSessionHost: SessionHost {
    private var serial: UInt32 = 0
    private var window: UInt64 = 0

    func post(_ event: SessionEvent) -> UInt32 {
        serial += 1
        return serial
    }

    func addWindow(width: Int32, height: Int32) -> UInt64 {
        window += 1
        return window
    }

    func setGate(_ window: UInt64, open: Bool) {}
    func removeWindow(_ window: UInt64) {}
    func setBackground(_ on: Bool) {}
    func waitScheduled(seconds: Double) -> Bool { true }

    static func wait(serial: UInt32, seconds: Double) -> Int32 { 1 }

    /// Nothing draws without a guest; session-check has the Debug build count presents of its own.
    static func presents(_ window: UInt64) -> UInt64 {
        SessionCheck.presents(window)
    }
}
#endif

/// The motion events until Runtime's input channel exists (ADR-0036, step b): the Debug build shows
/// session-check what reaches it, and nothing goes further.
final class NoChannelMotionSink: MotionEventSink {
    func post(_ events: [MotionEvent]) {
        #if DEBUG
        events.forEach(SessionCheck.motion)
        #endif
    }
}
