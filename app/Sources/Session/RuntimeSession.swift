import APKRuntime
import APKSession
import Foundation
import QuartzCore
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

/// ADR-0028's session and window registry (`apkrt.h`) for the bridge: the events go to activity-host,
/// and the window is the view's CAMetalLayer.
final class RuntimeSessionHost: SessionHost {
    /// The layer every window of the session is on, once the view exists; the registry retains it while a
    /// window is there. The bridge adds windows only after the first layout.
    var layer: CAMetalLayer?
    /// The frame clock's `setPreferredRate`: the request handler calls it on the guest's thread, and a
    /// window's removal with 0.
    private let preferRate: @Sendable (Float) -> Void
    #if BENCHMARK
    /// The presents of the session's removed windows, whose own count is 0 once they are gone: a recorded
    /// session compares the guest's presents with the layer's drawables (ADR-0041 d).
    private(set) var presentsOfRemovedWindows: UInt64 = 0
    #endif

    init(preferRate: @escaping @Sendable (Float) -> Void) {
        self.preferRate = preferRate
    }

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

    /// No FRAME_RATE request for the window comes once it is removed, and its preference goes with it: a
    /// new window has none, as a new Surface on Android (ADR-0036, decision 2).
    func removeWindow(_ window: UInt64) {
        #if BENCHMARK
        // The gate is closed by now, so no present comes between the count and the removal.
        presentsOfRemovedWindows += apkrt_window_presents(window)
        #endif
        _ = apkrt_window_remove(window)
        preferRate(0)
    }

    func setBackground(_ on: Bool) {
        apkrt_set_background(on ? 1 : 0)
    }

    func waitScheduled(seconds: Double) -> Bool {
        apkrt_wait_scheduled(seconds) == 1
    }

    func addInputChannel() -> UInt64 {
        apkrt_input_add()
    }

    /// A turn's new size (ADR-0038); the bridge never gives a size below 1 for a window it has.
    func setWindowSize(_ window: UInt64, width: Int32, height: Int32) {
        _ = apkrt_window_set_size(window, width, height)
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
        case .configurationChanged: (APKRT_EVENT_CONFIGURATION_CHANGED, 0, 0, 0)
        }
    }

    /// `low | high << 32`, 32 bits each (`guest_interface.h`).
    private static func pair(_ low: Int32, _ high: Int32) -> UInt64 {
        UInt64(UInt32(bitPattern: low)) | UInt64(UInt32(bitPattern: high)) << 32
    }

    /// The paths of decision 14 and the session with its configuration and handlers, before activity-host
    /// starts (ADR-0028 d); the handlers get this host as their context. Nil, or why the app cannot start.
    func begin(_ start: GuestStart, configuration: GuestConfiguration) -> String? {
        var error = [CChar](repeating: 0, count: 1024)
        for path in start.mounts where apkrt_mount(path.guest, path.host, &error, error.count) != 0 {
            return Self.text(error)
        }
        for path in start.libraryDirectories
        where apkrt_add_library_directory(path.guest, path.host, &error, error.count) != 0 {
            return Self.text(error)
        }
        var session = apkrt_session()
        session.context = Unmanaged.passUnretained(self).toOpaque()
        session.end = Self.endHandler
        session.request = Self.requestHandler
        session.configuration = Self.record(configuration)
        guard apkrt_session_begin(&session) == 0 else { return "the session did not begin" }
        return nil
    }

    /// activity-host, once the session has begun; not on the main thread, where it would map the program.
    /// Nil, or why it cannot start.
    static func startProgram(_ start: GuestStart) -> String? {
        _ = graphicsProviders
        var error = [CChar](repeating: 0, count: 1024)
        let result = withCStrings(start.arguments) { arguments in
            withCStrings(start.environment) { environment in
                apkrt_start_program(start.program, arguments, environment, &error, error.count)
            }
        }
        return result == 0 ? nil : text(error)
    }

    /// The providers of the graphics front libs' host tables (ADR-0030, decision 3): ANGLE, and MoltenVK on the
    /// iPhone, from the app's Frameworks. Once per process, before the program starts, so the front libs find
    /// their drivers and "scheduled" asks both at the background. The Simulator has no MoltenVK: NULL leaves
    /// Vulkan without a provider there, and it answers "no driver".
    private static let graphicsProviders: Void = {
        let frameworks = Bundle.main.privateFrameworksURL
        let angle = frameworks?.appending(path: "libGLESv2.framework/libGLESv2").path
        #if targetEnvironment(simulator)
        let moltenVK: String? = nil
        #else
        let moltenVK = frameworks?.appending(path: "MoltenVK.framework/MoltenVK").path
        #endif
        if minato_graphics_set_providers(moltenVK, angle) != 0 {
            log.error("the graphics providers were refused: MoltenVK \(moltenVK ?? "none", privacy: .public), ANGLE \(angle ?? "none", privacy: .public)")
        }
    }()

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
        case APKRT_REQUEST_FRAME_RATE.rawValue: .frameRate(bits: argument)
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
    /// request goes to the main queue; the text lives only until it returns, so it is copied first. A frame
    /// rate goes to the clock at once, which takes it without blocking: the runtime reports none for a
    /// window after its removal has returned, so the preference cannot come after the reset.
    static let requestHandler: apkrt_request_handler = { context, type, argument, text in
        guard let request = RuntimeSessionHost.request(type: type, argument: argument, text: text) else { return }
        if case .frameRate(let rate) = request, let context {
            Unmanaged<RuntimeSessionHost>.fromOpaque(context).takeUnretainedValue().preferRate(rate)
        }
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
    func addInputChannel() -> UInt64 { 0 }
    func setWindowSize(_ window: UInt64, width: Int32, height: Int32) {}

    static func wait(serial: UInt32, seconds: Double) -> Int32 { 1 }

    /// Nothing draws without a guest; session-check has the Debug build count presents of its own.
    static func presents(_ window: UInt64) -> UInt64 {
        SessionCheck.presents(window)
    }
}
#endif

/// The motion events to the guest's input channel (ADR-0036, decision 3); the Debug build shows
/// session-check each, with the posts the channel did not take so far.
final class InputChannelSink: MotionEventSink {
    /// The channel of the guest's input queue, 0 before it has one.
    var channel: UInt64 = 0
    #if DEBUG
    private var failedPosts = 0
    #endif

    func post(_ events: [MotionEvent]) {
        for event in events {
            let record = event.record
            let places = record.places.map {
                apkrt_pointer_coords(x: $0.x, y: $0.y, pressure: $0.pressure, touch_major: $0.touchMajor)
            }
            let posted = apkrt_input_post(
                channel, record.action, record.downTime, UInt32(record.pointerIDs.count), record.pointerIDs,
                UInt32(record.sampleTimes.count), record.sampleTimes, places) == 1
            #if DEBUG
            if !posted { failedPosts += 1 }
            SessionCheck.motion(event, channel: channel, failedPosts: failedPosts)
            #endif
        }
    }
}
