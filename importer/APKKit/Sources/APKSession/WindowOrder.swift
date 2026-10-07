/// Where a layer lies among the session's layers (ADR-0041's second windows, docs/plans/adr-0041-second-windows.md,
/// decision 1), as Android 15 orders an app's windows:
///   - the activities' groups by the guest's key, the session's window in group 0, and the toasts above them;
///   - in a group, its base window lowest and a starting window highest, the others newer above
///     (ActivityRecord.isFirstChildWindowGreaterThanSecond);
///   - around each window, its children (c4's surfaces and its sub-windows) by sublayer and then newer above, as c4
///     orders surfaces: below the window for a negative sublayer, above it otherwise;
///   - one dim, below the topmost window that dims and below that window's children, as Android 15's Dimmer applies
///     only the topmost dimming container's.
public enum WindowOrder {
    /// A layer of the session's view.
    public enum Layer: Hashable, Sendable {
        /// The session's window, M3's full-screen layer.
        case sessionWindow
        /// A SurfaceView's surface of c4, by its window's ID.
        case surface(UInt64)
        /// A requested window, by its token.
        case window(UInt64)
        /// The dim below the topmost window that dims.
        case dim
    }

    /// A layer besides the session's window, with what places it.
    public struct Entry: Sendable, Equatable {
        public enum Kind: Sendable, Equatable {
            /// c4's surface: a child of the session's window, with its z as its sublayer.
            case surface(z: Int32)
            /// A requested window: `LayoutParams.type`; a sub-window's parent, a requested window's token or 0 for
            /// the session's window; an application window's group; and whether it dims.
            case window(type: Int32, parent: UInt64, group: UInt32, dims: Bool)
        }

        /// A surface's window ID, or a requested window's token.
        public var id: UInt64
        public var kind: Kind
        /// The number of its make among all of the tree's makes, surfaces and windows alike.
        public var made: UInt64

        public static func surface(id: UInt64, z: Int32, made: UInt64) -> Entry {
            Entry(id: id, kind: .surface(z: z), made: made)
        }

        public static func window(token: UInt64, type: Int32, parent: UInt64 = 0, group: UInt32 = 0,
                                  dims: Bool = false, made: UInt64) -> Entry {
            Entry(id: token, kind: .window(type: type, parent: parent, group: group, dims: dims), made: made)
        }
    }

    // WindowManager.LayoutParams' types.
    private static let baseApplication: Int32 = 1
    private static let applicationStarting: Int32 = 3
    private static let subWindows: ClosedRange<Int32> = 1000...1999
    private static let firstSystemWindow: Int32 = 2000

    /// The session's layers, bottom to top. A sub-window whose parent the entries lack lies among the session's
    /// window's children; the runtime keeps a sub-window's parent live, so only a host's own slip gets there.
    public static func layers(_ entries: [Entry]) -> [Layer] {
        var parents = Set<UInt64>()
        for entry in entries {
            if case .window(let type, _, _, _) = entry.kind, !subWindows.contains(type) { parents.insert(entry.id) }
        }
        var children: [UInt64: [Entry]] = [:]  // by the parent's token, 0 for the session's window
        var groups: [UInt32: [Entry]] = [0: []]
        var systemWindows: [Entry] = []
        for entry in entries {
            switch entry.kind {
            case .surface:
                children[0, default: []].append(entry)
            case .window(let type, let parent, let group, _):
                if subWindows.contains(type) {
                    children[parents.contains(parent) ? parent : 0, default: []].append(entry)
                } else if type >= firstSystemWindow {
                    systemWindows.append(entry)
                } else {
                    groups[group, default: []].append(entry)
                }
            }
        }

        var result: [Layer] = []
        var starts: [Layer: Int] = [:]  // where each window's layers begin, its children below it among them
        var topmostDimming: Layer?

        func place(_ layer: Layer, among own: [Entry], dims: Bool) {
            starts[layer] = result.count
            let sorted = own.sorted { (childSublayer($0), $0.made) < (childSublayer($1), $1.made) }
            for child in sorted where childSublayer(child) < 0 { place(child) }
            result.append(layer)
            if dims { topmostDimming = layer }
            for child in sorted where childSublayer(child) >= 0 { place(child) }
        }
        func place(_ entry: Entry) {
            switch entry.kind {
            case .surface:
                result.append(.surface(entry.id))
            case .window(_, _, _, let dims):
                place(.window(entry.id), among: children[entry.id] ?? [], dims: dims)
            }
        }

        for group in groups.keys.sorted() {
            if group == 0 { place(.sessionWindow, among: children[0] ?? [], dims: false) }
            for entry in groups[group, default: []].sorted(by: { (rank($0), $0.made) < (rank($1), $1.made) }) {
                place(entry)
            }
        }
        for entry in systemWindows.sorted(by: { $0.made < $1.made }) { place(entry) }
        if let topmostDimming, let start = starts[topmostDimming] { result.insert(.dim, at: start) }
        return result
    }

    /// A sub-window type's sublayer (WindowManagerPolicy.getSubWindowLayerFromTypeLw, Android 15), which logs an
    /// unknown type and takes 0.
    public static func sublayer(type: Int32) -> Int32 {
        switch type {
        case 1001: -2  // TYPE_APPLICATION_MEDIA
        case 1004: -1  // TYPE_APPLICATION_MEDIA_OVERLAY
        case 1000, 1003: 1  // TYPE_APPLICATION_PANEL, TYPE_APPLICATION_ATTACHED_DIALOG
        case 1002: 2  // TYPE_APPLICATION_SUB_PANEL
        case 1005: 3  // TYPE_APPLICATION_ABOVE_SUB_PANEL
        default: 0
        }
    }

    private static func childSublayer(_ entry: Entry) -> Int32 {
        switch entry.kind {
        case .surface(let z): z
        case .window(let type, _, _, _): sublayer(type: type)
        }
    }

    /// An application window's place in its group: the base window lowest, a starting window highest.
    private static func rank(_ entry: Entry) -> Int {
        guard case .window(let type, _, _, _) = entry.kind else { return 1 }
        return type == baseApplication ? 0 : type == applicationStarting ? 2 : 1
    }
}
