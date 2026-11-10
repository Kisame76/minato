/// When the starting window goes (ADR-0076 decision 1): at the first frame of the starting activity's window, whoever
/// draws it (a software post, eglSwapBuffers, vkQueuePresent). That is the session's window, into which an opaque
/// activity draws, also a trampoline's next one, or an activity's own requested window (type 1: a floating starting
/// activity's, or a translucent next one's; SessionBridge.activityWindows). A dialog, a toast, a pop-up or a
/// SurfaceView's surface does not take it away, and nothing else does: an app that holds its first frame back (a
/// pre-draw listener) keeps it on screen, as on Android.
public struct StartingWindowWatch: Sendable {
    /// Whether it went.
    public private(set) var isGone = false

    public init() {}

    /// Called at each frame clock tick with the windows that count (0 for none yet): the first of them with a present
    /// takes the starting window away; that window, once.
    public mutating func check(_ windows: [UInt64], presents: (UInt64) -> UInt64) -> UInt64? {
        guard !isGone, let window = windows.first(where: { $0 != 0 && presents($0) > 0 }) else { return nil }
        isGone = true
        return window
    }
}
