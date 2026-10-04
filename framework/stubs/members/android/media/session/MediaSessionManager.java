// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// PhoneWindow's media and volume keys; the media module's sources are not fetched.
class MediaSessionManager {
    /** @hide */
    public boolean dispatchMediaKeyEventToSessionAsSystemService(android.view.KeyEvent keyEvent,
            MediaSession.Token sessionToken) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void dispatchVolumeKeyEventAsSystemService(android.view.KeyEvent keyEvent, int streamType) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void dispatchVolumeKeyEventToSessionAsSystemService(android.view.KeyEvent keyEvent,
            MediaSession.Token sessionToken) {
        throw new RuntimeException("stub");
    }
}
