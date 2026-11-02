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

    /// The radii of the screen's corners, clockwise from the top left: points in a Housing, pixels in the insets.
    public struct Corners: Sendable, Equatable {
        public var topLeft: Int32
        public var topRight: Int32
        public var bottomRight: Int32
        public var bottomLeft: Int32

        public init(topLeft: Int32, topRight: Int32, bottomRight: Int32, bottomLeft: Int32) {
            self.topLeft = topLeft
            self.topRight = topRight
            self.bottomRight = bottomRight
            self.bottomLeft = bottomLeft
        }
    }

    /// What iOS does not say about a model, from the amendment's table (ADR-0041, decision 8 changed): the sensor
    /// housing's edge in portrait or nil for none, its length along that edge in points [guess], and the corners'
    /// radii in portrait or nil for none, which Android allows.
    public struct Housing: Sendable, Equatable {
        public var cutout: Edge?
        public var cutoutLength: Double
        public var corners: Corners?

        /// `model` is the hardware's identifier, as "iPhone19,4". The iPhone Duo's sensor bar is on the right, its
        /// hinge's corners 8 pt and the outer ones 59 (folded; open [to verify]); an iPad has no cutout; every other
        /// iPhone, an unknown one too, has the Dynamic Island at the top [its 126 pt a guess], without corners.
        public init(model: String) {
            if model == "iPhone19,4" {
                cutout = .right
                cutoutLength = 100
                corners = Corners(topLeft: 8, topRight: 59, bottomRight: 59, bottomLeft: 8)
            } else if model.hasPrefix("iPad") {
                cutout = nil
                cutoutLength = 0
                corners = nil
            } else {
                cutout = .top
                cutoutLength = 126
                corners = nil
            }
        }

        /// The housing of the model the process runs on, from `model(environment:machine:)`.
        public init(environment: [String: String], machine: String?) {
            self.init(model: Self.model(environment: environment, machine: machine))
        }

        /// The model's identifier, the only thing read of the device: the Simulator's SIMULATOR_MODEL_IDENTIFIER (its
        /// hw.machine is the Mac's), else sysctl's hw.machine, else "", which the table takes for an unknown iPhone.
        public static func model(environment: [String: String], machine: String?) -> String {
            if let simulated = environment["SIMULATOR_MODEL_IDENTIFIER"], !simulated.isEmpty {
                return simulated
            }
            return machine ?? ""
        }
    }

    /// The device's housing at the screen's scale.
    public struct Device: Sendable, Equatable {
        public var cutout: Edge?
        public var cutoutLength: Double
        public var corners: Corners?
        public var scale: Double

        public init(cutout: Edge?, cutoutLength: Double, corners: Corners? = nil, scale: Double) {
            self.cutout = cutout
            self.cutoutLength = cutoutLength
            self.corners = corners
            self.scale = scale
        }

        public init(housing: Housing, scale: Double) {
            self.init(cutout: housing.cutout, cutoutLength: housing.cutoutLength, corners: housing.corners,
                      scale: scale)
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
    /// `WindowInsets.getRoundedCorner`'s radii, turned with the interface, or nil for none.
    public var roundedCorners: Corners?

    public init(safeArea: SafeArea, size: PixelSize, orientation: InterfaceOrientations, device: Device) {
        let pixels = { (points: Double) in Int32((points * device.scale).rounded()) }
        let bottom = pixels(safeArea.bottom)
        let side = pixels(30)
        systemGestures = Edges(left: side, top: 0, right: side, bottom: bottom)
        mandatorySystemGestures = Edges(left: 0, top: 0, right: 0, bottom: bottom)
        cutout = nil
        // The device's portrait edges and corners turned with the interface: in landscapeRight its top is on the
        // screen's left.
        let turns = orientation == .landscapeLeft ? 1 : orientation == .landscapeRight ? 3 : 0
        roundedCorners = device.corners.map { corners in
            var radii = [Int32](repeating: 0, count: 4)
            for (index, radius) in [corners.topLeft, corners.topRight, corners.bottomRight, corners.bottomLeft]
                .enumerated() {
                radii[(index + turns) % 4] = pixels(Double(radius))
            }
            return Corners(topLeft: radii[0], topRight: radii[1], bottomRight: radii[2], bottomLeft: radii[3])
        }
        guard let natural = device.cutout else { return }
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
