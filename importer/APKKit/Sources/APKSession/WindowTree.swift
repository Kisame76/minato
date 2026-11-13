/// The iOS host's tree of the session's layers besides its window (ADR-0041, plan step 6 (c)): c4's surfaces
/// and the requested windows as they are made, placed and removed, each with the number of its make among all of
/// them, in WindowOrder's order (decision 1). The session's view keeps the layers below the session's window in one
/// container and those above it in another (SessionWindow.swift's SurfaceLayers), so the order comes split there, the
/// dim among them; the dim's amount is the topmost dimming window's.
public struct WindowTree: Sendable {
    private var entries: [WindowOrder.Entry] = []
    private var dims: [UInt64: Float] = [:]  // by a requested window's token
    private var made: UInt64 = 0

    public init() {}

    /// A c4 surface, by its window's ID, with its z as its sublayer.
    public mutating func addSurface(id: UInt64, z: Int32) {
        made += 1
        entries.append(.surface(id: id, z: z, made: made))
    }

    /// A surface's new z; nothing for an unknown surface.
    public mutating func placeSurface(id: UInt64, z: Int32) {
        guard let index = entries.firstIndex(where: { $0.id == id && $0.isSurface }) else { return }
        entries[index].kind = .surface(z: z)
    }

    /// A requested window, by its token: `LayoutParams.type`, its parent (a requested window's token, 0 for the
    /// session's window), its group and its dim (dimAmount with FLAG_DIM_BEHIND, else 0).
    public mutating func addWindow(token: UInt64, type: Int32, parent: UInt64, group: UInt32, dim: Float) {
        made += 1
        entries.append(.window(token: token, type: type, parent: parent, group: group, dims: dim > 0, made: made))
        dims[token] = dim
    }

    /// A requested window's new dim; nothing for an unknown window.
    public mutating func placeWindow(token: UInt64, dim: Float) {
        guard let index = entries.firstIndex(where: { $0.id == token && !$0.isSurface }),
              case .window(let type, let parent, let group, _) = entries[index].kind else { return }
        entries[index].kind = .window(type: type, parent: parent, group: group, dims: dim > 0)
        dims[token] = dim
    }

    /// A surface or a requested window that went.
    public mutating func remove(_ layer: WindowOrder.Layer) {
        switch layer {
        case .surface(let id):
            entries.removeAll { $0.id == id && $0.isSurface }
        case .window(let token):
            entries.removeAll { $0.id == token && !$0.isSurface }
            dims[token] = nil
        case .sessionWindow, .dim:
            break
        }
    }

    /// The layers bottom to top, below the session's window and above it.
    public var split: (below: [WindowOrder.Layer], above: [WindowOrder.Layer]) {
        let layers = WindowOrder.layers(entries)
        guard let window = layers.firstIndex(of: .sessionWindow) else { return ([], layers) }
        return (Array(layers[..<window]), Array(layers[(window + 1)...]))
    }

    /// The topmost dimming window's dim, 0 without one.
    public var dimAmount: Float {
        let layers = WindowOrder.layers(entries)
        guard let dim = layers.firstIndex(of: .dim), dim + 1 < layers.count,
              case .window(let token) = layers[dim + 1] else { return 0 }
        return dims[token] ?? 0
    }
}

extension WindowOrder.Entry {
    fileprivate var isSurface: Bool {
        if case .surface = kind { return true }
        return false
    }
}
