#if DEBUG
import APKLibrary
import APKSession
import Darwin
import Foundation
import Synchronization
import UIKit

/// Answers tools/session-check.sh in the Simulator and on the iPhone (ADR-0032, test 6). A restarted process
/// gets no launch arguments, so the script leaves its requests as files in tmp/session-check/, which every process
/// reads; nothing happens without that folder. Each process appends what it does to
/// tmp/session-check/events, one line per event: "<seconds since 1970> <pid> <event>".
///   way      "url", "workspace" or "none": the only way both modes restart (Restart.requestedWay).
///   tap      a package: the library taps that app once its entries are loaded, and removes the file.
///   quit     game mode opens its menu 1.5 s after its start, or 0.5 s plus the seconds the file names,
///            and chooses Quit 1.5 s later.
///   confirm  the fallback's alert, once it is up, answers OK by itself after the seconds the file names
///            (1 when it names none), which ends the process.
///   orientation  "landscape" or "portrait": game mode holds that instead of the app's own.
///   unanswered   the next restart's first request goes unanswered, so it times out; the file is removed.
///   forget-hint  the next process forgets that game mode showed its hint and that the FPS label is on, as
///            before the first session, and removes the file.
///   crash    an exit status: game mode ends at its start as the runtime ends a program, reporting that
///            status, and removes the file.
///   start-failed  a reason: game mode's app reports that it cannot start, and the file is removed.
///   tone     game mode plays a test tone through the audio host for the seconds the file names (2 when it
///            names none), then logs the frames its render block read.
///   fps      a rate, or "hide": without a guest, every window presents that many frames a second, and game
///            mode turns the FPS label on through its menu; "hide" turns it off there.
///   no-guest game mode runs without a guest, as before ADR-0028 d): every post and wait is answered at once.
///   guest-log  the guest's log (its stderr, logcat's tag format) goes to tmp/session-check/guest-log, and its
///            stdout to tmp/session-check/guest-out: a Java app's System.out, which Android sends to the log.
///   extra-session  once the guest starts, game mode builds a second GameSession for the same app and keeps
///            it, as SwiftUI may build a view's state more than once; it must change nothing.
///   turn-library  the library asks its scene to turn to landscape, as a turned device would, logs whether iOS
///            refused and the orientation a second later, and removes the file (ADR-0038, decision 1.1).
///   turn-session  2 s after the guest's start, game mode asks its scene to turn to landscape, as a turned
///            device would, then a second after that layout back to portrait, logging the orientation and the
///            window's pixels after each, and removes the file (ADR-0038, test 3).
///   status-bar  game mode shows the status bar, for the Game Mode check of ADR-0038 step e.
///   audio-disconnect  that many seconds after the guest's start (1 for "yes"), the audio host disconnects every
///            open stream, as a reroute that changes the rate does, for audio-live's error callback; the file goes.
///   resize   <width>x<height> in pixels: 2 s after the guest's start, game mode takes that size as a layout would,
///            as the iPhone Duo's fold gives one (ADR-0041, decision 12), and logs the guest's size and the letterbox;
///            the file goes. The view itself keeps its size.
///   compile-hold  seconds: compile mode waits that long before dex2oat starts, with its screen up and its
///            background task held, so that a check sees the screen, cancels or goes to the background meanwhile.
///   no-compile  game mode's sessions ask for no compile mode, so that the next guest log is the app's own, not
///            dex2oat's (ADR-0053, M6 c, E5); compile mode itself still runs for a request already there.
///   footprint-log  every 10 s from the guest's start the process's physical footprint, the memory iOS counts
///            against it, and every iOS memory warning, one line each in tmp/session-check/footprint-log:
///            "<seconds since 1970> footprint <bytes>" and "<seconds since 1970> warning <bytes>".
///   ping     once a second while game mode runs: when the file is there, it goes and the event "pong presents <n>
///            scene <state>" says that the host's main thread answers, how many frames the guest's windows have
///            presented so far and the scene's state (the resume round of tools/java-start-check.sh).
///   menu     the same poll opens game mode's menu, logs "menu opened" and closes it 0.5 s later, as the three-finger
///            gesture and the menu's Continue do; the file goes.
///   continue  the menu that waits over the app at a return from the background (ADR-0032 decision 7, the menu's
///            pause) chooses Continue after the seconds the file names (1 when it names none) and logs "menu
///            continued"; the file stays for every return.
///   window-crc  with guest-log, graphics/host writes a line per post of a window drawn by the CPU, with the
///            CRC-32 of its pixels (minato_graphics_set_window_crc), into the guest log from the guest's start on
///            (ADR-0043 b5, step f).
///   window-by-drawables  graphics/host draws the windows' buffers into drawables, as before ADR-0043 b5, from the
///            guest's start on (minato_graphics_set_window_by_drawables), for step f's footprint comparison.
///   window-by-contents  graphics/host shows the windows' buffers as contents in the Simulator too, the iPhone's
///            way, from the guest's start on (minato_graphics_set_window_by_contents).
///   window-dump  the poll has graphics/host dump each window's last shown buffer into
///            tmp/session-check/window-dump-files/ (minato_graphics_window_dump), logs "window dump <n>" and removes
///            the file (step f's screenshots compare with it).
///   pick-allow  a save's or a folder's alert (ADR-0057 step d) chooses Save or Allow by itself 1 s after it is up,
///            as the user's tap does, and logs "pick allowed"; the file stays for every alert.
///   end      a package: the library removes that app, then the whole folder, which ends the check. On the
///            iPhone the script cannot delete files in the app's container itself.
/// A request's file says "yes" when it names nothing; an empty file is none.
enum SessionCheck {
    static let folder = URL.temporaryDirectory.appending(path: "session-check", directoryHint: .isDirectory)

    private static var isOn: Bool { FileManager.default.fileExists(atPath: folder.path) }

    /// An empty file is no request: on the iPhone the script cannot delete one, only empty it.
    private static func requested(_ name: String) -> Bool {
        text(of: name) != nil
    }

    private static func text(of name: String) -> String? {
        guard isOn, let text = try? String(contentsOf: folder.appending(path: name), encoding: .utf8) else { return nil }
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }

    /// Whether game mode runs without a guest.
    static var withoutGuest: Bool { requested("no-guest") }

    /// Whether game mode's sessions ask for no compile mode.
    static var withoutCompile: Bool { requested("no-compile") }

    /// Whether a save's or a folder's alert answers by itself (ADR-0057 step d).
    static var allowsPicks: Bool { requested("pick-allow") }

    /// How long compile mode waits before dex2oat starts; 0 without the request.
    static var compileHold: Double { text(of: "compile-hold").flatMap(Double.init) ?? 0 }

    /// Whether game mode shows the status bar (ADR-0038, step e).
    static var showsStatusBar: Bool { requested("status-bar") }

    /// The guest is about to start: its stderr, which the runtime hands it as descriptor 2, goes to the
    /// file the script reads, and its stdout, descriptor 1, to guest-out (ADR-0039 c5).
    static func guestStarts() {
        if requested("window-by-drawables"), minato_graphics_set_window_by_drawables(1) == 0 {
            event("window by drawables")
        }
        if requested("window-by-contents"), minato_graphics_set_window_by_contents(1) == 0 {
            event("window by contents")
        }
        guard requested("guest-log") else { return }
        for (name, descriptor) in [("guest-log", Int32(2)), ("guest-out", Int32(1))] {
            let file = open(folder.appending(path: name).path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0o644)
            guard file >= 0 else { return event("\(name) failed") }
            dup2(file, descriptor)
            close(file)
        }
        event("guest log on")
        if requested("window-crc"), minato_graphics_set_window_crc("-") == 0 {
            event("window crc on")
        }
        startFootprintLog()
    }

    /// Where the footprint log's sampler lives while the process runs.
    private static let footprintTimer = Mutex<DispatchSourceTimer?>(nil)

    /// The footprint-log request: a line now and every 10 s, and one per memory warning, in the file the script reads.
    private static func startFootprintLog() {
        guard requested("footprint-log") else { return }
        let file = folder.appending(path: "footprint-log")
        try? Data().write(to: file)
        @Sendable func line(_ kind: String) {
            let text = String(format: "%.3f", Date.now.timeIntervalSince1970) + " \(kind) \(physFootprint)\n"
            guard let handle = try? FileHandle(forWritingTo: file) else { return }
            defer { try? handle.close() }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: Data(text.utf8))
        }
        _ = NotificationCenter.default.addObserver(
            forName: UIApplication.didReceiveMemoryWarningNotification, object: nil, queue: nil
        ) { _ in
            line("warning")
            event("memory warning")
        }
        let timer = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        timer.schedule(deadline: .now(), repeating: 10)
        timer.setEventHandler { line("footprint") }
        timer.resume()
        footprintTimer.withLock { $0 = timer }
    }

    /// The process's physical footprint in bytes, -1 when the kernel does not say.
    private static var physFootprint: Int64 {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
        let result = withUnsafeMutablePointer(to: &info) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return result == KERN_SUCCESS ? Int64(info.phys_footprint) : -1
    }

    /// The way the script asks for.
    static var requestedWay: Restart.Way? { text(of: "way").flatMap(Restart.Way.init) }

    /// The orientation the script asks game mode to hold.
    static var requestedOrientation: SessionOrientation? {
        switch text(of: "orientation") {
        case "landscape": .landscape
        case "portrait": .portrait
        default: nil
        }
    }

    /// Whether the restart's first request goes unanswered, once.
    @MainActor
    static func dropsAnswer() -> Bool {
        guard requested("unanswered") else { return false }
        try? FileManager.default.removeItem(at: folder.appending(path: "unanswered"))
        event("answer dropped")
        return true
    }

    /// Events come from the main thread, the audio host's queue and guest threads: one at a time, so that no
    /// line overwrites another at the file's end, and in the order of their times.
    private static let writing = Mutex(())

    static func event(_ text: String) {
        guard isOn else { return }
        writing.withLock { _ in
            let line = String(format: "%.3f", Date.now.timeIntervalSince1970) + " \(getpid()) \(text)\n"
            let file = folder.appending(path: "events")
            if let handle = try? FileHandle(forWritingTo: file) {
                defer { try? handle.close() }
                _ = try? handle.seekToEnd()
                try? handle.write(contentsOf: Data(line.utf8))
            } else {
                try? Data(line.utf8).write(to: file)
            }
        }
    }

    /// The mode chosen at launch, and the seconds since the launch request was written.
    static func processStarted(_ mode: LaunchMode, request: LaunchRequest?) {
        if requested("forget-hint") {
            UserDefaults.standard.removeObject(forKey: GameSession.hintShownKey)
            UserDefaults.standard.removeObject(forKey: GameSession.showsFPSKey)
            try? FileManager.default.removeItem(at: folder.appending(path: "forget-hint"))
        }
        let seconds = request.map { String(format: " %.3f", Date.now.timeIntervalSince($0.time)) } ?? ""
        switch mode {
        case .gameMode(let entry): event("process gameMode \(entry.id)\(seconds)")
        case .compileMode(let entry): event("process compileMode \(entry.id)\(seconds)")
        case .library(nil): event("process library\(seconds)")
        case .library(.appNotFound(let package)): event("process library appNotFound \(package)\(seconds)")
        case .library(.appNotReady(let entry)): event("process library appNotReady \(entry.id)\(seconds)")
        case .library(.quitUnexpectedly(let notice)):
            switch notice.cause {
            case .signal(let number): event("process library quitUnexpectedly \(notice.name) signal \(number)\(seconds)")
            case .failure(let reason): event("process library quitUnexpectedly \(notice.name) failure \(reason)\(seconds)")
            case .endedByIOS: event("process library quitUnexpectedly \(notice.name) endedByIOS\(seconds)")
            case .exitStatus(let status):
                event("process library quitUnexpectedly \(notice.name) status \(status)\(seconds)")
            }
        }
    }

    /// The library taps the requested app once, as the user would.
    @MainActor
    static func tapIfRequested(in library: LibraryModel) async {
        let file = folder.appending(path: "tap")
        guard let package = text(of: "tap"), let entry = library.entries.first(where: { $0.id == package }) else { return }
        try? FileManager.default.removeItem(at: file)
        event("tap \(package)")
        switch await library.launch(entry) {
        case nil: break
        case .openSigning: event("blocked openSigning")
        case .explain(_, let reason): event("blocked \(reason)")
        }
    }

    /// The library asks to turn to landscape once; in portrait only, iOS refuses and the scene stays portrait.
    @MainActor
    static func turnLibraryIfRequested() async {
        guard requested("turn-library"),
              let scene = UIApplication.shared.connectedScenes.first(where: { $0 is UIWindowScene }) as? UIWindowScene
        else { return }
        try? FileManager.default.removeItem(at: folder.appending(path: "turn-library"))
        scene.requestGeometryUpdate(.iOS(interfaceOrientations: .landscape)) { @Sendable error in
            event("library turn refused \(error.localizedDescription)")
        }
        try? await Task.sleep(for: .seconds(1))
        event("library orientation \(orientationName(scene.effectiveGeometry.interfaceOrientation))")
    }

    /// Turns game mode's scene to landscape and back, as the emulator's reference turns the device
    /// (tools/android-reference.sh): rotation-test is ready a second after its first focus.
    @MainActor
    private static func turn(_ session: GameSession) {
        try? FileManager.default.removeItem(at: folder.appending(path: "turn-session"))
        guard let scene = UIApplication.shared.connectedScenes.first(where: { $0 is UIWindowScene }) as? UIWindowScene
        else { return event("session turn without a scene") }
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(2))
            await turn(scene, of: session, to: .landscape)
            try? await Task.sleep(for: .seconds(1))
            await turn(scene, of: session, to: .portrait)
        }
    }

    /// Asks `scene` for `mask` and waits at most 3 s for the window's layout in it.
    @MainActor
    private static func turn(_ scene: UIWindowScene, of session: GameSession, to mask: UIInterfaceOrientationMask) async {
        scene.requestGeometryUpdate(.iOS(interfaceOrientations: mask)) { @Sendable error in
            event("session turn refused \(error.localizedDescription)")
        }
        let wide = mask == .landscape
        for _ in 0..<30 where (session.windowPixels.width > session.windowPixels.height) != wide {
            try? await Task.sleep(for: .seconds(0.1))
        }
        event("session orientation \(orientationName(scene.effectiveGeometry.interfaceOrientation)) "
            + "window \(Int(session.windowPixels.width))x\(Int(session.windowPixels.height))")
    }

    /// Hands game mode's session the size `text` names, 2 s after the guest's start, as a fold's layout would.
    @MainActor
    private static func resize(_ session: GameSession, to text: String) {
        try? FileManager.default.removeItem(at: folder.appending(path: "resize"))
        let parts = text.split(separator: "x").compactMap { Double($0) }
        guard parts.count == 2, parts.allSatisfy({ $0 > 0 }) else { return event("resize malformed \(text)") }
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(2))
            session.windowLaidOut(CGSize(width: parts[0], height: parts[1]))
            event("resize \(text) \(session.sizeLine)")
        }
    }

    private static func orientationName(_ orientation: UIInterfaceOrientation) -> String {
        switch orientation {
        case .portrait: "portrait"
        case .portraitUpsideDown: "portraitUpsideDown"
        case .landscapeLeft: "landscapeLeft"
        case .landscapeRight: "landscapeRight"
        default: "unknown"
        }
    }

    /// The check is over: the library removes the requested apps, one package per line, then the folder.
    @MainActor
    static func endIfRequested(in library: LibraryModel) async {
        guard let text = text(of: "end") else { return }
        let packages = Set(text.split(whereSeparator: \.isNewline).map(String.init))
        let entries = library.entries.filter { packages.contains($0.id) }
        if !entries.isEmpty { await library.remove(entries) }
        try? FileManager.default.removeItem(at: folder)
    }

    /// The sessions "extra-session" built, kept for the process's life.
    @MainActor private static var extraSessions: [GameSession] = []

    /// Game mode's guest starts: the window's size in pixels it starts with, the window's facts, then the
    /// menu and Quit when the script asks for them.
    @MainActor
    static func gameModeStarted(_ session: GameSession) {
        guard isOn else { return }
        event("start window \(Int(session.windowPixels.width))x\(Int(session.windowPixels.height))")
        event("layer opaque \(session.layerIsOpaque ? 1 : 0)")
        if let status = text(of: "crash").flatMap(Int32.init) {
            try? FileManager.default.removeItem(at: folder.appending(path: "crash"))
            event("crash \(status)")
            SessionEndReport.endAsTheRuntimeWould(status)
        }
        if let status = text(of: "self-kill").flatMap(Int32.init) {
            try? FileManager.default.removeItem(at: folder.appending(path: "self-kill"))
            event("self-kill \(status)")
            // As the runtime ends a program that kills itself (ADR-0028, decision 5): its report, then the end handler.
            SessionEndReport.reportAsTheRuntimeWould(status)
            session.programEnded(status: status)
        }
        if let reason = text(of: "start-failed") {
            try? FileManager.default.removeItem(at: folder.appending(path: "start-failed"))
            event("start failed \(reason)")
            session.received(.startFailed(reason))
        }
        if requested("tone") { playTone(session.audio, seconds: text(of: "tone").flatMap(Double.init) ?? 2) }
        if requested("turn-session") { turn(session) }
        if let size = text(of: "resize") { resize(session, to: size) }
        if requested("audio-disconnect") {
            let seconds = text(of: "audio-disconnect").flatMap(Double.init) ?? 1
            try? FileManager.default.removeItem(at: folder.appending(path: "audio-disconnect"))
            Task { @MainActor in
                try? await Task.sleep(for: .seconds(seconds))
                event("audio disconnect forced")
                session.audio.disconnectAll()
            }
        }
        poll(session)
        if requested("extra-session") {
            extraSessions.append(GameSession(entry: session.entry))
            event("extra session made")
        }
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(0.5))
            recordWindow()
            if let fps = text(of: "fps") {
                event("fps at start \(session.showsFPS ? "shown" : "hidden")")
                // As the user would: the menu, then its button, which closes it.
                if (fps == "hide") == session.showsFPS {
                    session.openMenu()
                    try? await Task.sleep(for: .seconds(0.5))
                    session.showFPS(!session.showsFPS)
                    session.closeMenu()
                }
            }
            guard requested("quit") else { return }
            try? await Task.sleep(for: .seconds(text(of: "quit").flatMap(Double.init) ?? 1))
            session.openMenu()
            try? await Task.sleep(for: .seconds(1.5))
            session.quit()
        }
    }

    /// The ping, window-dump and menu requests, which come while the session runs: once a second the files are
    /// looked for.
    @MainActor
    private static func poll(_ session: GameSession) {
        Task { @MainActor [weak session] in
            while let session {
                try? await Task.sleep(for: .seconds(1))
                if requested("ping") {
                    try? FileManager.default.removeItem(at: folder.appending(path: "ping"))
                    let state = switch UIApplication.shared.applicationState {
                    case .active: "active"
                    case .inactive: "inactive"
                    default: "background"
                    }
                    event("pong presents \(session.allPresents) scene \(state)")
                }
                if requested("window-dump") {
                    try? FileManager.default.removeItem(at: folder.appending(path: "window-dump"))
                    let files = folder.appending(path: "window-dump-files").path
                    event("window dump \(minato_graphics_window_dump(files))")
                }
                if requested("menu") {
                    try? FileManager.default.removeItem(at: folder.appending(path: "menu"))
                    session.openMenu()
                    event("menu opened")
                    try? await Task.sleep(for: .seconds(0.5))
                    session.closeMenu()
                }
            }
        }
    }

    /// Back from the background with the menu over the app: Continue, as the user would.
    @MainActor
    static func returned(to session: GameSession) {
        guard requested("continue") else { return }
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(text(of: "continue").flatMap(Double.init) ?? 1))
            guard session.menuIsOpen else { return }
            event("menu continued")
            session.closeMenu()
        }
    }

    /// Where each window's fake presents started counting.
    private static let presentsStart = Mutex<[UInt64: Double]>([:])

    /// The presents of `window` so far when the script asks for a rate: that rate since the first look.
    static func presents(_ window: UInt64) -> UInt64 {
        guard window != 0, let rate = text(of: "fps").flatMap(Double.init) else { return 0 }
        let now = Double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) / 1e9
        let start = presentsStart.withLock { starts in
            if let start = starts[window] { return start }
            starts[window] = now
            return now
        }
        return UInt64((now - start) * rate)
    }

    /// A 440 Hz tone at 44.1 kHz, so the mixer converts it to the hardware's rate, in a LOW_LATENCY stream.
    @MainActor
    private static func playTone(_ audio: AudioHost, seconds: Double) {
        let tone = ToneSource(sampleRate: 44_100, frequency: 440)
        // The runtime's handles are at least 64, a generation above the slot (runtime/src/audio.cpp): 1 is none.
        let id: UInt64 = 1
        guard let (stream, hardware) = audio.open(stream: id, sampleRate: 44_100, channels: 2, mode: .lowLatency,
                                                  source: tone)
        else { return event("audio tone failed") }
        event("audio tone opened \(id) rate \(Int(hardware.sampleRate)) burst \(hardware.burstFrames)")
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(seconds))
            audio.close(id)
            event("audio tone frames \(tone.frames.load(ordering: .relaxed)) expected \(Int(44_100 * seconds)) xruns \(stream.xruns)")
        }
    }

    /// What the audio host did, with the hardware after it: rate, IO buffer in ms, burst in frames.
    static func audio(_ action: AudioOutput.Action, hardware: AudioHardware) {
        event("audio \(action) rate \(Int(hardware.sampleRate)) ioBuffer "
            + String(format: "%.2f", hardware.ioBufferDuration * 1000) + " burst \(hardware.burstFrames)")
    }

    /// A motion event that reached the sink: its action, its pointers' IDs and its history's rows.
    /// A motion event the input channel was given, and how many it did not take so far; then each pointer's
    /// pressure and touch major in pixels.
    static func motion(_ motion: MotionEvent, channel: UInt64, failedPosts: Int) {
        let pointers = motion.pointers.map { String(format: "%.2f/%.0f", $0.pressure, $0.touchMajor) }.joined(separator: ",")
        event("motion \(motion.action) ids \(motion.pointers.map { String($0.id) }.joined(separator: ",")) history \(motion.history.count)"
              + " channel \(channel) failed \(failedPosts) pressure/major \(pointers)")
    }

    /// ADR-0036's guess that `UITouch.timestamp` counts on CLOCK_UPTIME_RAW, the guest's CLOCK_MONOTONIC:
    /// a new touch's time before now on that clock and on CLOCK_MONOTONIC_RAW, which also counts sleep, in
    /// ms. Once the machine has slept, only the uptime clock's gap is small.
    static func touchClock(_ timestamp: TimeInterval) {
        let uptime = Double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) / 1e9
        let monotonic = Double(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)) / 1e9
        event(String(format: "touch clock uptime %.3f monotonic %.3f", (uptime - timestamp) * 1000,
                     (monotonic - timestamp) * 1000))
    }

    @MainActor private static var sceneObservers: [NSObjectProtocol] = []

    /// The scene's changes of state as UIKit posts them, for the time from the focus's loss to the background
    /// (ADR-0036 a): a frame that commits after the background waits at the gate until the foreground.
    @MainActor
    static func observeScene() {
        guard isOn, sceneObservers.isEmpty else { return }
        let names: [(Notification.Name, String)] = [
            (UIScene.willDeactivateNotification, "willDeactivate"),
            (UIScene.didEnterBackgroundNotification, "didEnterBackground"),
            (UIScene.willEnterForegroundNotification, "willEnterForeground"),
            (UIScene.didActivateNotification, "didActivate"),
        ]
        sceneObservers = names.map { name, text in
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: nil) { _ in event("scene \(text)") }
        }
    }

    /// The wait for the guest's answer to `callback`, begun at `start` on CLOCK_UPTIME_RAW, ended with
    /// `apkrt_session_wait`'s `result`: whether it was answered, and how long it took in ms.
    static func answer(_ callback: Lifecycle.Callback, result: Int32, since start: UInt64) {
        answer(callback, result: result, seconds: Double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1e9)
    }

    /// The same for a wait of `seconds`, which the bridge ran itself at the background.
    static func answer(_ callback: Lifecycle.Callback, result: Int32, seconds: Double) {
        let ms = seconds * 1000
        let outcome = switch result {
        case 1: "answered"
        case 0: "timedOut"
        default: "broken"
        }
        event("answer \(callback) \(outcome) " + String(format: "%.0f", ms))
    }

    /// 120 intervals between ticks of the frame clock, in ms: smallest, median and largest, the period
    /// the last tick had, how many were late, longer than 1.5 periods, and whether they came on the main thread.
    static func frames(_ intervals: [Int64], period: Int64, onMain: Bool) -> String {
        let sorted = intervals.sorted()
        let late = intervals.count { $0 * 2 > period * 3 }
        func ms(_ ns: Int64) -> String { String(format: "%.2f", Double(ns) / 1e6) }
        return "frames \(intervals.count) interval \(ms(sorted.first ?? 0))/\(ms(sorted[sorted.count / 2]))/\(ms(sorted.last ?? 0)) "
            + "period \(ms(period)) late \(late) main \(onMain)"
    }

    /// The restart failed: whether the fallback's alert comes up, with its title, and with the request
    /// "confirm" its OK.
    @MainActor
    static func restartFailed() {
        guard isOn else { return }
        Task { @MainActor in
            var title: String?
            for _ in 0..<50 where title == nil {
                try? await Task.sleep(for: .seconds(0.1))
                title = alertTitle()
            }
            event(title.map { "fallback shown \($0)" } ?? "fallback not shown, on top: \(topController())")
            guard requested("confirm") else { return }
            try? await Task.sleep(for: .seconds(text(of: "confirm").flatMap(Double.init) ?? 1))
            event("fallback confirmed")
            _exit(0)
        }
    }

    /// The title of the alert on top of the window, if one is up.
    @MainActor
    private static func alertTitle() -> String? {
        guard let alert = topmost() as? UIAlertController, alert.preferredStyle == .alert else { return nil }
        return alert.title
    }

    @MainActor
    private static func topController() -> String {
        topmost().map { String(describing: type(of: $0)) } ?? "nothing"
    }

    /// The view controller on top of the key window; UpdateCheck reads alerts through it too.
    @MainActor
    static func topmost() -> UIViewController? {
        guard let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene,
              let window = scene.windows.first(where: \.isKeyWindow) ?? scene.windows.first
        else { return nil }
        var controller = window.rootViewController
        while let presented = controller?.presentedViewController { controller = presented }
        return controller
    }

    /// The layer's size in pixels and whether it covers the window, the screen's, the orientation, and
    /// what the window asks of the system: status bar, home indicator, and the edges that defer gestures.
    @MainActor
    private static func recordWindow() {
        guard let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene,
              let window = scene.windows.first(where: \.isKeyWindow) ?? scene.windows.first,
              let root = window.rootViewController
        else { return event("window none") }
        let host = metalLayerView(in: window)
        let pixels = host.map { "\(Int($0.bounds.width * $0.layer.contentsScale))x\(Int($0.bounds.height * $0.layer.contentsScale))" } ?? "none"
        let covers = host.map { $0.convert($0.bounds, to: window) == window.bounds } ?? false
        let screen = scene.screen
        let edges = deciding(root) { $0.childForScreenEdgesDeferringSystemGestures }.preferredScreenEdgesDeferringSystemGestures
        let homeIndicator = deciding(root) { $0.childForHomeIndicatorAutoHidden }.prefersHomeIndicatorAutoHidden
        event("window layer \(pixels) covers \(covers) screen \(Int(screen.nativeBounds.width))x\(Int(screen.nativeBounds.height)) "
            + "scale \(host?.layer.contentsScale ?? 0)/\(screen.nativeScale) orientation \(scene.effectiveGeometry.interfaceOrientation.rawValue) "
            + "statusBarHidden \(scene.statusBarManager?.isStatusBarHidden ?? false) homeIndicatorHidden \(homeIndicator) "
            + "deferredEdges \(edges.rawValue)")
    }

    /// The view controller UIKit asks, following the children a controller hands the question to.
    @MainActor
    private static func deciding(_ controller: UIViewController, child: (UIViewController) -> UIViewController?) -> UIViewController {
        var current = controller
        while let next = child(current), next !== current { current = next }
        return current
    }

    @MainActor
    private static func metalLayerView(in view: UIView) -> UIView? {
        if view.layer is CAMetalLayer { return view }
        for subview in view.subviews {
            if let found = metalLayerView(in: subview) { return found }
        }
        return nil
    }
}

/// session-check's test tone, a render source that never runs dry. Its phase is only touched on the
/// real-time thread.
private final class ToneSource: AudioRenderSource, @unchecked Sendable {
    let frames = Atomic<Int>(0)
    private let step: Double
    private var phase = 0.0

    init(sampleRate: Double, frequency: Double) {
        step = 2 * .pi * frequency / sampleRate
    }

    func read(into buffer: UnsafeMutablePointer<Float>, frames count: Int, channels: Int, hostTime: UInt64) -> Int {
        for frame in 0..<count {
            let value = Float(sin(phase)) * 0.2
            for channel in 0..<channels { buffer[frame * channels + channel] = value }
            phase += step
            if phase >= 2 * .pi { phase -= 2 * .pi }
        }
        frames.add(count, ordering: .relaxed)
        return count
    }
}
#endif
