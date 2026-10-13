import Foundation

/// The libraries an app cannot start without (ADR-0055, decision 2): a NativeActivity's own and the core libraries of
/// the engines the analyzer knows. A library of these that cannot load blocks the app; any other only degrades.
public enum StartPath {
    /// Their file names among `libraries`.
    public static func libraries(of report: APKReport) -> Set<String> {
        var names: Set<String> = []
        if case .nativeActivity(let library) = report.runtime { names.insert("lib\(library).so") }
        let present = Set(report.nativeLibraries.map(\.name))
        // Unity's, Godot's, Unreal's, Flutter's, GameMaker's, libGDX's and SDL's core libraries; Cocos2d-x's all.
        let core = ["libunity.so", "libil2cpp.so", "libgodot_android.so", "libUE4.so", "libUnreal.so",
                    "libflutter.so", "libapp.so", "libgdx.so", "libyoyo.so", "libSDL2.so", "libSDL3.so"]
        names.formUnion(present.intersection(core))
        // libmain.so is Unity's and SDL's own start library, but only there: another app may call a library that.
        if !present.isDisjoint(with: ["libunity.so", "libSDL2.so", "libSDL3.so"]) && present.contains("libmain.so") {
            names.insert("libmain.so")
        }
        names.formUnion(present.filter { $0.hasPrefix("libcocos") })
        return names
    }
}
