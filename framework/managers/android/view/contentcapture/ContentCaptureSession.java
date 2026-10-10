// The written part of ContentCaptureSession (the Unity test APK probe: View's fields name the type, and a class whose fields
// cannot be resolved cannot be read by reflection), which tools/managergen joins with the API's methods. Content capture
// stays off on Minato (ADR-0045 decision 4): no Context gives a ContentCaptureManager, as on a device without the
// service, so no session is ever made, and the class is the API with the members View and ContentCaptureManager name.
// A session that is made anyway is disabled and drops what it is told.
class ContentCaptureSession {
    /** @hide */
    public @interface FlushReason {
    }

    /** @hide */
    void start(android.os.IBinder token, android.os.IBinder shareableActivityToken,
            android.content.ComponentName component, int flags) {
    }

    /** @hide */
    void flush(int reason) {
    }

    /** @hide */
    boolean isDisabled() {
        return true;
    }

    /** @hide */
    boolean setDisabled(boolean disabled) {
        return true;
    }

    /** @hide */
    void dump(String prefix, java.io.PrintWriter pw) {
    }

    /** @hide */
    public int getId() {
        return 0;
    }

    /** @hide */
    public void notifyViewTreeEvent(boolean started) {
    }
}
