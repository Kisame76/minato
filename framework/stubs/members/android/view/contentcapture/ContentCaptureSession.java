// Hidden members of the public API's compile-only stub (ADR-0039 step c2c1), which tools/managergen joins with it:
// content capture stays off on Minato: no Context gives a ContentCaptureManager, as on a device without the service;
// ContentCaptureManager.flush names its reason.
// (ADR-0039 step c2c1) ContentCaptureManager starts, flushes, disables and dumps its main session.
class ContentCaptureSession {
    /** @hide */
    public @interface FlushReason {
    }

    /** @hide */
    void start(android.os.IBinder token, android.os.IBinder shareableActivityToken,
            android.content.ComponentName component, int flags) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    void flush(int reason) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    boolean isDisabled() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    boolean setDisabled(boolean disabled) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    void dump(String prefix, java.io.PrintWriter pw) {
        throw new RuntimeException("stub");
    }
}
