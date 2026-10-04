#if BENCHMARK
import APKBench
import Darwin
import SwiftUI
import UIKit

/// Test 2's twin (ADR-0042, decision 3, step c): Godot's iOS template with our wrapper in MinatoGodot.framework, which
/// only benchmark builds for iOS devices embed (tools/embed-benchmark.sh). It is opened by dlopen in a twin run only
/// (BenchLaunch.mayOpenTwin): such a process shows the run alone and ends after it, so the library never runs beside
/// Godot, which keeps one engine per process and swizzles the root view controller's class.
struct GodotTwin {
    typealias Start = @convention(c) (Int32, UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?) -> Int32
    typealias ViewController = @convention(c) () -> UnsafeMutableRawPointer?
    typealias Before = @convention(c) (UnsafeMutableRawPointer?) -> Void
    typealias After = @convention(c) (UnsafeMutableRawPointer?, Int, Int32) -> Void
    typealias FrameHooks = @convention(c) (Before?, After?, UnsafeMutableRawPointer?) -> Int32

    let start: Start
    let viewController: ViewController
    /// minato_godot_frame_hooks: `before` and `after` each frame of Godot's view, on the main thread.
    let frameHooks: FrameHooks

    static func open() throws -> GodotTwin {
        guard BenchLaunch.mayOpenTwin(ProcessInfo.processInfo.arguments) else {
            throw BenchmarkError("the twin opens in a twin run only")
        }
        let path = Bundle.main.privateFrameworksURL?.appending(path: "MinatoGodot.framework/MinatoGodot").path ?? ""
        guard let handle = dlopen(path, RTLD_NOW | RTLD_LOCAL) else {
            let reason = dlerror().map { String(cString: $0) } ?? "no reason"
            throw BenchmarkError("MinatoGodot.framework could not be opened: \(reason)")
        }
        func symbol<T>(_ name: String, as type: T.Type) throws -> T {
            guard let address = dlsym(handle, name) else { throw BenchmarkError("MinatoGodot has no \(name)") }
            return unsafeBitCast(address, to: type)
        }
        return GodotTwin(start: try symbol("minato_godot_start", as: Start.self),
                         viewController: try symbol("minato_godot_view_controller", as: ViewController.self),
                         frameHooks: try symbol("minato_godot_frame_hooks", as: FrameHooks.self))
    }

    /// Sets Godot up on the main thread, with the executable's path first as in its own app, and returns its view
    /// controller, before that view first draws. Godot keeps the arguments for the process, which ends after the run.
    @MainActor
    func run(_ arguments: [String]) throws -> UIViewController {
        let strings = ([Bundle.main.executablePath ?? "/Minato"] + arguments).map { strdup($0) }
        let argv = UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>.allocate(capacity: strings.count + 1)
        argv.initialize(from: strings + [nil], count: strings.count + 1)
        let status = start(Int32(strings.count), argv)
        guard status == 0 else { throw BenchmarkError("Godot did not start: Main::setup returned \(status)") }
        guard let controller = viewController() else { throw BenchmarkError("Godot gave no view controller") }
        return Unmanaged<UIViewController>.fromOpaque(controller).takeRetainedValue()
    }
}

/// Godot's view controller as the run's screen.
struct GodotTwinView: UIViewControllerRepresentable {
    let controller: UIViewController

    func makeUIViewController(context: Context) -> UIViewController { controller }

    func updateUIViewController(_ controller: UIViewController, context: Context) {}
}
#endif
