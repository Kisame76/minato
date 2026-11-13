/// A window of the guest's own (ADR-0041): a dialog's, a popup's or a toast's, apkrt_window_request as the
/// runtime checked it (apkrt.h's window handler): per token a make, places and a remove, in that order.
public struct WindowRequest: Sendable, Equatable {
    public enum Operation: Sendable, Equatable {
        /// A window for a token that has none.
        case make
        /// The window's new frame, buffer size, alpha or dim; type, parent, group and format stay the make's.
        case place
        /// The guest is done with the window, also after WINDOW_DESTROYED; a sub-window's comes before its parent's.
        case remove
    }

    public var operation: Operation
    /// The guest's name of the window, one per live window of the session.
    public var token: UInt64
    /// LayoutParams.type: 1 to 99 an application window, 1000 to 1999 a sub-window, 2005 a toast.
    public var type: Int32
    /// A sub-window's window: a live requested one's token, or 0 for the session's window; 0 for every other type.
    public var parent: UInt64
    /// The frame in the session's pixels.
    public var frame: PixelRect
    /// The buffer's size in pixels: the frame's unless the app fixed one.
    public var size: PixelSize
    /// The format has no alpha (Android's PixelFormat.formatHasAlpha).
    public var opaque: Bool
    /// An application window's activity as a sort key: 0 on the session's window, larger above; 0 for sub-windows and
    /// toasts.
    public var group: UInt32
    /// LayoutParams.alpha, 0 to 1.
    public var alpha: Float
    /// dimAmount with FLAG_DIM_BEHIND, else 0, 0 to 1.
    public var dim: Float

    public init(operation: Operation, token: UInt64, type: Int32, parent: UInt64, frame: PixelRect, size: PixelSize,
                opaque: Bool, group: UInt32, alpha: Float, dim: Float) {
        self.operation = operation
        self.token = token
        self.type = type
        self.parent = parent
        self.frame = frame
        self.size = size
        self.opaque = opaque
        self.group = group
        self.alpha = alpha
        self.dim = dim
    }

    /// A sub-window, whose WINDOW_DESTROYED comes before its parent's.
    public var isSubWindow: Bool { (1000...1999).contains(type) }
    /// An activity's own window (TYPE_BASE_APPLICATION): a translucent or floating activity's (ViewRootImpl's
    /// requestActivityWindow), which an opaque activity's window on the session's window is not.
    public var isActivityWindow: Bool { type == 1 }
}
