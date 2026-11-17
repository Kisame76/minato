/// A rectangle in the window's pixels, as Android's Rect: left and top inside it, right and bottom just past it.
public struct PixelRect: Sendable, Equatable {
    public var left: Int32
    public var top: Int32
    public var right: Int32
    public var bottom: Int32

    public init(left: Int32, top: Int32, right: Int32, bottom: Int32) {
        self.left = left
        self.top = top
        self.right = right
        self.bottom = bottom
    }
}

/// A SurfaceView's request for its surface (ADR-0043 c4, `guest_interface.h`'s struct apkrt_surface_request), as the
/// runtime checked it before the host sees it: per token a make, places and a remove, in that order.
public struct SurfaceRequest: Sendable, Equatable {
    public enum Operation: Sendable, Equatable {
        /// A surface for a token that has none.
        case make
        /// The surface's new frame, z order or buffer size; its format stays the make's.
        case place
        /// The guest is done with the surface, also after SURFACE_DESTROYED.
        case remove
    }

    public var operation: Operation
    /// The SurfaceView's token, a new one for each make.
    public var token: UInt64
    /// The sublayer SurfaceView keeps: -2 media, -1 media overlay, 1 on top, 2 a sub-panel's.
    public var z: Int32
    /// The frame in the window's pixels.
    public var frame: PixelRect
    /// The buffer's size in pixels: the frame's unless the app fixed one.
    public var size: PixelSize
    /// The format has no alpha (Android's PixelFormat.formatHasAlpha).
    public var opaque: Bool

    public init(operation: Operation, token: UInt64, z: Int32, frame: PixelRect, size: PixelSize, opaque: Bool) {
        self.operation = operation
        self.token = token
        self.z = z
        self.frame = frame
        self.size = size
        self.opaque = opaque
    }
}
