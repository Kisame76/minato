/// The guest's insets from iOS's safe area, per edge, as Android 15 reports them under gesture navigation (ADR-0041,
/// decision 8), in the screen's pixels.
public struct GuestInsets: Sendable, Equatable {
    /// The screen's edges, clockwise from the top.
    public enum Edge: Int, Sendable {
        case top, right, bottom, left
    }

    /// iOS's safe area, in points.
    public struct SafeArea: Sendable, Equatable {
        public var top: Double
        public var left: Double
        public var bottom: Double
        public var right: Double

        public init(top: Double, left: Double, bottom: Double, right: Double) {
            self.top = top
            self.left = left
            self.bottom = bottom
            self.right = right
        }
    }

    /// What iOS does not say about the device: the cutout's edge in portrait (the Dynamic Island's top, the iPhone
    /// Duo's sensor bar on the right) or nil for none, its length along that edge in points [guess: a table per
    /// model], and the screen's scale.
    public struct Device: Sendable, Equatable {
        public var cutout: Edge?
        public var cutoutLength: Double
        public var scale: Double

        public init(cutout: Edge?, cutoutLength: Double, scale: Double) {
            self.cutout = cutout
            self.cutoutLength = cutoutLength
            self.scale = scale
        }
    }

    /// `displayCutout`: the safe area's inset on the cutout's edge, with one bounding rectangle centered on it.
    public struct Cutout: Sendable, Equatable {
        public var edge: Edge
        public var inset: Int32
        public var rect: PixelRect
    }

    public struct Edges: Sendable, Equatable {
        public var left: Int32
        public var top: Int32
        public var right: Int32
        public var bottom: Int32
    }

    public var cutout: Cutout?
    /// 30 dp left and right (decision 10) and the home indicator's inset at the bottom.
    public var systemGestures: Edges
    /// The home indicator's inset.
    public var mandatorySystemGestures: Edges

    public init(safeArea: SafeArea, size: PixelSize, orientation: InterfaceOrientations, device: Device) {
        let pixels = { (points: Double) in Int32((points * device.scale).rounded()) }
        let bottom = pixels(safeArea.bottom)
        let side = pixels(30)
        systemGestures = Edges(left: side, top: 0, right: side, bottom: bottom)
        mandatorySystemGestures = Edges(left: 0, top: 0, right: 0, bottom: bottom)
        cutout = nil
        guard let natural = device.cutout else { return }
        // The device's portrait edge turned with the interface: in landscapeRight its top is on the screen's left.
        let turns = orientation == .landscapeLeft ? 1 : orientation == .landscapeRight ? 3 : 0
        let edge = Edge(rawValue: (natural.rawValue + turns) % 4)!
        let inset = pixels([safeArea.top, safeArea.right, safeArea.bottom, safeArea.left][edge.rawValue])
        guard inset > 0 else { return }
        let length = pixels(device.cutoutLength)
        let rect: PixelRect
        switch edge {
        case .top, .bottom:
            let left = (size.width - length) / 2
            let top = edge == .top ? 0 : size.height - inset
            rect = PixelRect(left: left, top: top, right: left + length, bottom: top + inset)
        case .left, .right:
            let top = (size.height - length) / 2
            let left = edge == .left ? 0 : size.width - inset
            rect = PixelRect(left: left, top: top, right: left + inset, bottom: top + length)
        }
        cutout = Cutout(edge: edge, inset: inset, rect: rect)
    }
}
