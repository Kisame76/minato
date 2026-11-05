import APKRuntime
import APKSession
import Foundation
import QuartzCore
import UIKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

/// ADR-0028's session and window registry (`apkrt.h`) for the bridge: the events go to activity-host,
/// and the window is the view's CAMetalLayer.
final class RuntimeSessionHost: SessionHost {
    /// The layer every window of the session is on, once the view exists; the registry retains it while a
    /// window is there. The bridge adds windows only after the first layout.
    var layer: CAMetalLayer?
    /// The SurfaceViews' layers beside the window's (ADR-0043 c4), once the view exists.
    var surfaces: SurfaceLayers?
    /// The guest's wish for its window layer's opacity (ADR-0030 V5), which a WebView below the window overrides.
    var windowOpaque = true {
        didSet { updateWindowLayer() }
    }
    /// The windows on `layer`, whose WebViews graphics/host puts into the session's view (ADR-0056 c2).
    private var windows: Set<UInt64> = []
    /// How many WebViews each window shows, where it shows any.
    private var webViews: [UInt64: UInt32] = [:]
    /// The frame clock's `setPreferredRate`: the request handler calls it on the guest's thread, and a
    /// window's removal with 0.
    private let preferRate: @Sendable (Float) -> Void
    /// What the app finished in MediaStore, for Photos (ADR-0057 step e): the request handler takes the file at once.
    private let exports: MediaExports
    /// Where they go, which the session waits for before the process ends.
    let photos = PhotoExports.queue()
    #if BENCHMARK
    /// The presents of the session's removed windows, whose own count is 0 once they are gone: a recorded
    /// session compares the guest's presents with the layer's drawables (ADR-0041 d).
    private(set) var presentsOfRemovedWindows: UInt64 = 0
    #endif

    /// `dataFolder` is the app's data folder from the library's records (ApplicationInfo.dataDir).
    init(dataFolder: URL?, preferRate: @escaping @Sendable (Float) -> Void) {
        self.preferRate = preferRate
        exports = MediaExports(dataFolder: dataFolder)
    }

    func post(_ event: SessionEvent) -> UInt32 {
        let (type, a0, a1, a2) = Self.record(event)
        return apkrt_session_post(type.rawValue, a0, a1, a2)
    }

    func addWindow(width: Int32, height: Int32) -> UInt64 {
        let window = apkrt_window_add(layer.map { Unmanaged.passUnretained($0).toOpaque() }, width, height)
        if window != 0 { windows.insert(window) }
        // A NativeActivity's render surface is the session's window; a Java app's decor presents within ms.
        if window != 0 { apkrt_stall_surface_created(window) }
        noteWindow("window \(window) added (the session's window, \(width)x\(height))")
        return window
    }

    /// SessionBridge gives the window up: why, and the callbacks of that step, in a Debug build's session log.
    func windowGivenUp(_ window: UInt64, reason: String, callbacks: [Lifecycle.Callback]) {
        noteWindow("window \(window) given up (\(reason): \(callbacks.map { "\($0)" }.joined(separator: ", ")))")
    }

    /// A Debug build's session log line for a window that came or went, with its ID and the call (ADR-0043 b5): a
    /// refused post (libgui's "queueBuffer: error queuing buffer, -19") then names who removed its window.
    private func noteWindow(_ line: @autoclosure () -> String) {
        #if DEBUG
        fputs("I/MinatoSession: \(line())\n", stderr)
        #endif
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
        noteWindow("window \(window) removed (by removeWindow)")
        windows.remove(window)
        // Its WebViews go with it: the next window starts opaque, as the guest wants it.
        if webViews.removeValue(forKey: window) != nil { updateWindowLayer() }
        preferRate(0)
    }

    func setBackground(_ on: Bool) {
        apkrt_set_background(on ? 1 : 0)
        // No sensor updates in the background (ADR-0061 decision 3).
        MotionHost.shared.setBackground(on)
        // The app's edits of the user's files are written before iOS may suspend the process (ADR-0057's addendum).
        WriteBackHost.shared.setBackground(on)
    }

    func waitScheduled(seconds: Double) -> Bool {
        apkrt_wait_scheduled(seconds) == 1
    }

    /// The bridge's wait at the background, on the main thread within its budget (ADR-0032, decision 6).
    func waitAnswer(serial: UInt32, seconds: Double) -> Int32 {
        Self.wait(serial: serial, seconds: seconds)
    }

    func addInputChannel() -> UInt64 {
        apkrt_input_add()
    }

    /// A turn's new size (ADR-0038); the bridge never gives a size below 1 for a window it has.
    func setWindowSize(_ window: UInt64, width: Int32, height: Int32) {
        _ = apkrt_window_set_size(window, width, height)
    }

    /// The window's layer lets the WebViews below it through where the guest cleared its pixels (ADR-0056 c2): not
    /// opaque, and without the background of its own, which SurfaceLayers moves behind them, as behind a surface below.
    private func updateWindowLayer() {
        let below = !webViews.isEmpty
        layer?.isOpaque = windowOpaque && !below
        surfaces?.webViewsBelow = below
        #if DEBUG
        SessionCheck.event("webviews \(webViews.values.reduce(0, +)) layer opaque \(layer?.isOpaque == true ? 1 : 0)")
        #endif
    }

    /// graphics/host's way into the session's view for the WebViews (ADR-0056 c2), set when the session begins; its
    /// functions run on the main queue: a window on `layer` takes the WebViews just below the layer's own view, at the
    /// layer's scale. The context is the session's host.
    nonisolated(unsafe) private static let webViewWindows = UnsafeMutablePointer<minato_webview_windows>.allocate(
        capacity: 1)

    private static func setWebViewWindows(_ context: UnsafeMutableRawPointer?) {
        webViewWindows.initialize(to: minato_webview_windows(
            context: context,
            container: { context, window, above, scale in
                let address = UInt(bitPattern: context)
                let found: (container: UInt, view: UInt, scale: Double)? = MainActor.assumeIsolated {
                    guard let host = RuntimeSessionHost.from(address), host.windows.contains(window),
                          let layer = host.layer, let view = layer.delegate as? UIView,
                          let container = view.superview else { return nil }
                    // The window's pixels per container point: in size compat (ADR-0041, decision 12 b) the
                    // window's view shows them at the letterbox's scale.
                    return (UInt(bitPattern: Unmanaged.passUnretained(container).toOpaque()),
                            UInt(bitPattern: Unmanaged.passUnretained(view).toOpaque()),
                            Double(layer.contentsScale) / Double(view.transform.a))
                }
                guard let found else { return nil }
                above?.pointee = UnsafeMutableRawPointer(bitPattern: found.view)
                scale?.pointee = found.scale
                return UnsafeMutableRawPointer(bitPattern: found.container)
            },
            shown: { context, window, count in
                let address = UInt(bitPattern: context)
                MainActor.assumeIsolated {
                    guard let host = RuntimeSessionHost.from(address) else { return }
                    host.webViews[window] = count == 0 ? nil : count
                    host.updateWindowLayer()
                }
            }))
        minato_webview_set_windows(webViewWindows)
    }

    /// The windows' views moved or scaled without a new place from the guest (a new size, a letterbox): graphics/host
    /// places every WebView again.
    static func windowsMoved() {
        minato_webview_windows_changed()
    }

    /// The host whose address a C function's context carries, or nil for none.
    private static func from(_ address: UInt) -> RuntimeSessionHost? {
        UnsafeRawPointer(bitPattern: address).map { Unmanaged<RuntimeSessionHost>.fromOpaque($0).takeUnretainedValue() }
    }

    /// A SurfaceView's window on a layer of its own beside the window's (ADR-0043 c4); the registry retains the layer
    /// while the window is there. 0 before the view exists or when the registry has no room.
    func addSurface(frame: PixelRect, z: Int32, size: PixelSize, opaque: Bool) -> UInt64 {
        let window = surfaces?.add(frame: frame, z: z, opaque: opaque) { layer in
            apkrt_window_add(Unmanaged.passUnretained(layer).toOpaque(), size.width, size.height)
        } ?? 0
        // The app's render surface: the runtime's watchdog expects its first frame (stall_watchdog.h).
        if window != 0 { apkrt_stall_surface_created(window) }
        noteWindow("window \(window) added (a surface, \(size.width)x\(size.height))")
        return window
    }

    func placeSurface(_ window: UInt64, frame: PixelRect, z: Int32) {
        surfaces?.place(window, frame: frame, z: z)
    }

    /// Unlike the window's removal, the frame rate stays: APKRT_REQUEST_FRAME_RATE names no window, so a surface's
    /// preference is the session's (ADR-0043 c4 (a)'s named limit).
    func removeSurface(_ window: UInt64) {
        _ = apkrt_window_remove(window)
        noteWindow("window \(window) removed (by removeSurface)")
        surfaces?.remove(window)
    }

    /// A window of the guest's own (ADR-0041, gap 4) on a layer in the windows' order, of its buffer's size; the
    /// registry retains the layer while the window is there. 0 before the view exists or when the registry has no
    /// room.
    func addRequestedWindow(_ request: WindowRequest) -> UInt64 {
        let window = surfaces?.addWindow(request) { layer in
            apkrt_window_add(Unmanaged.passUnretained(layer).toOpaque(), request.size.width, request.size.height)
        } ?? 0
        let size = request.size
        noteWindow("window \(window) added (requested window \(request.token), \(size.width)x\(size.height))")
        return window
    }

    func placeRequestedWindow(_ window: UInt64, _ request: WindowRequest) {
        surfaces?.placeWindow(token: request.token, frame: request.frame, alpha: request.alpha, dim: request.dim)
    }

    /// The frame rate stays, as for a surface.
    func removeRequestedWindow(_ window: UInt64, token: UInt64) {
        _ = apkrt_window_remove(window)
        noteWindow("window \(window) removed (by removeRequestedWindow, token \(token))")
        surfaces?.removeWindow(token: token)
    }

    /// `apkrt_session_wait`, off the main thread but for the bridge's background wait: 1 answered, 0 timed out, -1 the
    /// channel broke or the program ended.
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
        case .trimMemory(let level): (APKRT_EVENT_TRIM_MEMORY, UInt64(level), 0, 0)
        case .inputQueueCreated(let channel): (APKRT_EVENT_INPUT_QUEUE_CREATED, channel, 0, 0)
        case .nativeWindowResized(let window, let width, let height):
            (APKRT_EVENT_NATIVE_WINDOW_RESIZED, window, UInt64(width), UInt64(height))
        case .contentRectChanged(let left, let top, let right, let bottom):
            (APKRT_EVENT_CONTENT_RECT_CHANGED, pair(left, top), pair(right, bottom), 0)
        case .nativeWindowRedrawNeeded(let window): (APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED, window, 0, 0)
        case .configurationChanged: (APKRT_EVENT_CONFIGURATION_CHANGED, 0, 0, 0)
        case .surfaceCreated(let token, let window, let width, let height):
            (APKRT_EVENT_SURFACE_CREATED, token, window, pair(width, height))
        case .surfaceResized(let token, let window, let width, let height):
            (APKRT_EVENT_SURFACE_RESIZED, token, window, pair(width, height))
        case .surfaceDestroyed(let token, let window): (APKRT_EVENT_SURFACE_DESTROYED, token, window, 0)
        case .windowCreated(let token, let window, let width, let height):
            (APKRT_EVENT_WINDOW_CREATED, token, window, pair(width, height))
        case .windowResized(let token, let window, let width, let height):
            (APKRT_EVENT_WINDOW_RESIZED, token, window, pair(width, height))
        case .windowDestroyed(let token, let window): (APKRT_EVENT_WINDOW_DESTROYED, token, window, 0)
        case .openResult(let id, let outcome): (APKRT_EVENT_OPEN_RESULT, id, outcome.resultCode, outcome.reason)
        // The result code as a 32-bit int in the low bits (guest_interface.h).
        case .pickResult(let id, let code, let count):
            (APKRT_EVENT_PICK_RESULT, id, UInt64(UInt32(bitPattern: code)), count)
        case .imeInsets(let bottom): (APKRT_EVENT_IME_INSETS, UInt64(max(bottom, 0)), 0, 0)
        case .back(let phase, let progress, let edge):
            (APKRT_EVENT_BACK, UInt64(phase.rawValue), UInt64(max(progress, 0)), UInt64(max(edge, 0)))
        case .imeHidden: (APKRT_EVENT_IME_HIDDEN, 0, 0, 0)
        case .barInsets(let top, let bottom): (APKRT_EVENT_BAR_INSETS, UInt64(max(top, 0)), UInt64(max(bottom, 0)), 0)
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
        // The sensors CoreMotion feeds, before the guest runs (ADR-0061 step 3).
        MotionHost.shared.register()
        var session = apkrt_session()
        session.context = Unmanaged.passUnretained(self).toOpaque()
        session.end = Self.endHandler
        session.request = Self.requestHandler
        session.surface = Self.surfaceHandler
        // The guest's dialogs, popups and toasts on layers of their own (ADR-0041, gap 4).
        session.window = Self.windowHandler
        // The guest's WebViews on graphics/host's WKWebView, in the session's view (ADR-0056 c2).
        session.webview = minato_webview_handle
        Self.setWebViewWindows(session.context)
        session.configuration = Self.record(configuration)
        guard apkrt_session_begin(&session) == 0 else { return "the session did not begin" }
        return nil
    }

    /// activity-host, once the session has begun; not on the main thread, where it would map the program.
    /// Nil, or why it cannot start.
    static func startProgram(_ start: GuestStart) -> String? {
        _ = graphicsProviders
        if let refusal = angleBackend(start) { return refusal }
        // Before the start, as the runtime keeps from then on what it has (ADR-0054 c2a).
        guard apkrt_set_internet(start.internet ? 1 : 0) == 0 else { return "the runtime's network permission could not be set" }
        // The app's profile, as the property area and the loader take it at the start (ADR-0065, ADR-0066).
        guard apkrt_set_device_profile(start.deviceProfile) == 0 else { return "the device profile could not be set" }
        guard apkrt_set_frame_rate_unlock(start.frameRateUnlock) == 0 else { return "the frame rate could not be set" }
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

    /// ADR-0044 d): the session's GLES on ANGLE's Vulkan backend for an app that asks for ES 3.1 or more
    /// (GuestStart.angleBackend), once graphics/host's run-time check passed; on Metal otherwise. A Debug build also
    /// takes MINATO_ANGLE_BACKEND=vulkan in tmp/guest-environment for an ES 3.0 app, and MINATO_ANGLE_CHECK=fail makes
    /// the check fail (test 2). Nil, or why the session cannot start, which the library shows.
    private static func angleBackend(_ start: GuestStart) -> String? {
        #if DEBUG
        let vulkan = start.angleBackend == .vulkan || start.environment.contains("MINATO_ANGLE_BACKEND=vulkan")
        let forced: Int32 = start.environment.contains("MINATO_ANGLE_CHECK=fail") ? 1 : 0
        #else
        let vulkan = start.angleBackend == .vulkan
        let forced: Int32 = 0
        #endif
        guard vulkan else { return nil }
        var reason = [CChar](repeating: 0, count: 256)
        guard minato_graphics_angle_vulkan_check(forced, &reason, reason.count) == 1 else {
            note("angle backend refused \(text(reason))")
            return "the ES 3.1 path is refused: \(text(reason))"
        }
        guard minato_graphics_set_angle_backend(1) == 0 else {
            note("angle backend refused without MoltenVK or patch 0009")
            return "the ES 3.1 path is refused: ANGLE has no MoltenVK"
        }
        note("angle backend vulkan \(text(reason))")
        return nil
    }

    /// The backend's line in the log, and in a Debug build the session's event for tools/session-check.sh.
    private static func note(_ line: String) {
        log.info("\(line, privacy: .public)")
        #if DEBUG
        SessionCheck.event(line)
        #endif
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
        case APKRT_REQUEST_FRAME_RATE.rawValue: .frameRate(bits: argument)
        case APKRT_REQUEST_WINDOW_OPAQUE.rawValue: .windowOpaque(argument != 0)
        // Without a text (the runtime always gives one) the request is still answered, as refused.
        case APKRT_REQUEST_OPEN.rawValue: .open(id: argument, text: text.map { String(cString: $0) } ?? "")
        case APKRT_REQUEST_OPEN_CANCEL.rawValue: .openCancel(id: argument)
        case APKRT_REQUEST_PICK.rawValue: .pick(id: argument, text: text.map { String(cString: $0) } ?? "")
        case APKRT_REQUEST_PICK_CANCEL.rawValue: .pickCancel(id: argument)
        case APKRT_REQUEST_IME.rawValue:
            .ime(serial: Int64(bitPattern: argument), text: text.map { String(cString: $0) } ?? "")
        case APKRT_REQUEST_BACK_AT_ROOT.rawValue: .backAtRoot
        case APKRT_REQUEST_ORIENTATION.rawValue: .orientation(argument: argument)
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

    /// The runtime's checked surface request as the bridge takes it (ADR-0043 c4), or nil for an op it does not know.
    static func surfaceRequest(_ record: apkrt_surface_request) -> SurfaceRequest? {
        let operation: SurfaceRequest.Operation
        switch record.op {
        case APKRT_SURFACE_MAKE.rawValue: operation = .make
        case APKRT_SURFACE_PLACE.rawValue: operation = .place
        case APKRT_SURFACE_REMOVE.rawValue: operation = .remove
        default: return nil
        }
        return SurfaceRequest(
            operation: operation, token: record.token, z: record.z,
            frame: PixelRect(left: record.left, top: record.top, right: record.right, bottom: record.bottom),
            size: PixelSize(width: record.width, height: record.height), opaque: record.opaque != 0)
    }

    /// The session's surface handler (ADR-0043 c4): like the request handler it runs on the guest thread that asks,
    /// here under the runtime's surface lock, and must not block, so a copy of the request goes to the main queue,
    /// where the requests keep the order the runtime took them in.
    static let surfaceHandler: apkrt_surface_handler = { _, record in
        guard let request = record.flatMap({ RuntimeSessionHost.surfaceRequest($0.pointee) }) else { return }
        DispatchQueue.main.async {
            MainActor.assumeIsolated { GameSession.current?.surfaceRequested(request) }
        }
    }

    /// The runtime's checked window request as the bridge takes it (ADR-0041, gap 4), or nil for an op it does not
    /// know.
    static func windowRequest(_ record: apkrt_window_request) -> WindowRequest? {
        let operation: WindowRequest.Operation
        switch record.op {
        case APKRT_WINDOW_MAKE.rawValue: operation = .make
        case APKRT_WINDOW_PLACE.rawValue: operation = .place
        case APKRT_WINDOW_REMOVE.rawValue: operation = .remove
        default: return nil
        }
        return WindowRequest(
            operation: operation, token: record.token, type: record.type, parent: record.parent,
            frame: PixelRect(left: record.left, top: record.top, right: record.right, bottom: record.bottom),
            size: PixelSize(width: record.width, height: record.height), opaque: record.opaque != 0,
            group: record.group, alpha: Float(bitPattern: record.alpha), dim: Float(bitPattern: record.dim))
    }

    /// The session's window handler (ADR-0041, gap 4), as the surface handler: on the guest thread under the
    /// runtime's window lock, so a copy of the request goes to the main queue, in the order the runtime took them.
    static let windowHandler: apkrt_window_handler = { _, record in
        guard let request = record.flatMap({ RuntimeSessionHost.windowRequest($0.pointee) }) else { return }
        DispatchQueue.main.async {
            MainActor.assumeIsolated { GameSession.current?.windowRequested(request) }
        }
    }

    /// The session's request handler: it runs on the guest thread that asks and must not block, so the
    /// request goes to the main queue; the text lives only until it returns, so it is copied first. A frame
    /// rate goes to the clock at once, which takes it without blocking: the runtime reports none for a
    /// window after its removal has returned, so the preference cannot come after the reset. An export to Photos
    /// is taken here, under the runtime's pick lock, and copied off this thread; it has no answer.
    static let requestHandler: apkrt_request_handler = { context, type, argument, text in
        if UInt32(bitPattern: type) == APKRT_REQUEST_EXPORT_MEDIA.rawValue {
            guard let context, let text else { return }
            let host = Unmanaged<RuntimeSessionHost>.fromOpaque(context).takeUnretainedValue()
            if !host.photos.declined, let job = host.exports.take(String(cString: text)) { host.photos.export(job) }
            return
        }
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
    func waitAnswer(serial: UInt32, seconds: Double) -> Int32 { Self.wait(serial: serial, seconds: seconds) }
    func addInputChannel() -> UInt64 { 0 }
    func setWindowSize(_ window: UInt64, width: Int32, height: Int32) {}

    func addSurface(frame: PixelRect, z: Int32, size: PixelSize, opaque: Bool) -> UInt64 {
        window += 1
        return window
    }

    func placeSurface(_ window: UInt64, frame: PixelRect, z: Int32) {}
    func removeSurface(_ window: UInt64) {}

    func addRequestedWindow(_ request: WindowRequest) -> UInt64 {
        window += 1
        return window
    }

    func placeRequestedWindow(_ window: UInt64, _ request: WindowRequest) {}
    func removeRequestedWindow(_ window: UInt64, token: UInt64) {}

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

/// The hardware keyboard's device and keys to the guest's input channel (ADR-0068 step 7c2): the first key adds the
/// device, whose name the channel takes only as BMP UTF-8 within its bytes (`HardwareKeyboard.deviceName`). KeyDelivery
/// offers the device again until a channel has taken it.
final class KeyChannelSink: KeyPoster {
    private lazy var delivery = KeyDelivery(poster: self)

    /// The channel of the guest's input queue, 0 before it has one.
    var channel: UInt64 {
        get { delivery.channel }
        set { delivery.channel = newValue }
    }

    func post(_ events: [KeyboardEvent]) {
        delivery.post(events)
    }

    func postDevice(channel: UInt64, _ device: KeyboardDevice) -> Bool {
        var record = apkrt_device_event()
        record.op = Int32(APKRT_DEVICE_ADDED)
        record.device_id = device.id
        record.sources = device.sources
        let name = Array(HardwareKeyboard.deviceName(device.name).utf8CString)
        withUnsafeMutableBytes(of: &record.name) { bytes in
            for (index, byte) in name.prefix(bytes.count).enumerated() {
                bytes[index] = UInt8(bitPattern: byte)
            }
        }
        return apkrt_input_post_device(channel, &record) == 1
    }

    func postKey(channel: UInt64, _ key: KeyRecord) -> Bool {
        var record = apkrt_key_event()
        record.action = key.action
        record.key_code = key.keyCode
        record.meta_state = key.metaState
        record.repeat_count = key.repeatCount
        record.down_time = key.downTime
        record.event_time = key.eventTime
        record.device_id = key.deviceID
        record.source = key.source
        return apkrt_input_post_key(channel, &record) == 1
    }
}

/// The game controllers' devices, keys and axes to the guest's input channel (ADR-0068 decision 5). PadDelivery offers
/// a pad again until a channel has taken it; its key records go as the keyboard's do (KeyChannelSink).
final class PadChannelSink: PadPoster {
    private lazy var delivery = PadDelivery(poster: self)
    private let keys = KeyChannelSink()

    /// The channel of the guest's input queue, 0 before it has one.
    var channel: UInt64 {
        get { delivery.channel }
        set { delivery.channel = newValue }
    }

    func post(_ events: [PadEvent]) {
        delivery.post(events)
    }

    /// The channel came: the pads that are there join the guest's devices.
    func offerAll() {
        delivery.offerAll()
    }

    func postDevice(channel: UInt64, _ device: GamepadDevice, added: Bool) -> Bool {
        var record = apkrt_device_event()
        record.op = Int32(added ? APKRT_DEVICE_ADDED : APKRT_DEVICE_REMOVED)
        record.device_id = device.id
        record.sources = device.sources
        record.vendor_id = device.vendorID
        record.product_id = device.productID
        let name = Array(HardwareKeyboard.deviceName(device.name).utf8CString)
        withUnsafeMutableBytes(of: &record.name) { bytes in
            for (index, byte) in name.prefix(bytes.count).enumerated() {
                bytes[index] = UInt8(bitPattern: byte)
            }
        }
        return apkrt_input_post_device(channel, &record) == 1
    }

    func postKey(channel: UInt64, _ key: KeyRecord) -> Bool {
        keys.postKey(channel: channel, key)
    }

    func postAxes(channel: UInt64, _ axes: AxesRecord) -> Bool {
        var record = apkrt_axes_event()
        record.action = axes.action
        record.event_time = axes.eventTime
        record.device_id = axes.deviceID
        record.source = axes.source
        let values = axes.axes.prefix(Int(APKRT_AXES_MAX))
        record.axis_count = UInt32(values.count)
        withUnsafeMutableBytes(of: &record.axes) { bytes in
            let slots = bytes.bindMemory(to: apkrt_axis_value.self)
            for (index, value) in values.enumerated() {
                slots[index] = apkrt_axis_value(axis: value.axis, value: value.value)
            }
        }
        return apkrt_input_post_axes(channel, &record) == 1
    }
}

/// The soft keyboard's text records to the guest's input channel (ADR-0068 step 2c). A record the channel refuses (no
/// channel yet, or a full one) is dropped, as a key is: the guest cannot merge edits.
final class TextChannelSink {
    /// The channel of the guest's input queue, 0 before it has one.
    var channel: UInt64 = 0

    func post(_ records: [TextRecord]) {
        guard channel != 0 else { return }
        for text in records {
            var record = apkrt_text_event()
            record.op = switch text.op {
            case .commit: Int32(APKRT_TEXT_COMMIT)
            case .deleteSurrounding: Int32(APKRT_TEXT_DELETE_SURROUNDING)
            case .setSelection: Int32(APKRT_TEXT_SET_SELECTION)
            case .performAction: Int32(APKRT_TEXT_PERFORM_ACTION)
            case .sendKey: Int32(APKRT_TEXT_SEND_KEY)
            }
            record.arg0 = text.arg0
            record.arg1 = text.arg1
            record.serial = text.serial
            let bytes = Array(text.text.utf8.prefix(Int(APKRT_TEXT_BYTES)))
            record.length = UInt32(bytes.count)
            withUnsafeMutableBytes(of: &record.text) { $0.copyBytes(from: bytes) }
            _ = apkrt_input_post_text(channel, &record)
        }
    }
}
