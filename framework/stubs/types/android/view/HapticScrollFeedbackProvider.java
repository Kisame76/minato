package android.view;

/** Compile-only stub (ADR-0039 step c2c2), never in framework.jar: View's rotary encoder scroll haptics. */
public class HapticScrollFeedbackProvider {
    public HapticScrollFeedbackProvider( View view) {
        throw new RuntimeException("stub");
    }

    public HapticScrollFeedbackProvider( View view, ViewConfiguration viewConfig, boolean disabledIfViewPlaysScrollHaptics) {
        throw new RuntimeException("stub");
    }

    public void onScrollProgress(int inputDeviceId, int source, int axis, int deltaInPixels) {
        throw new RuntimeException("stub");
    }

    public void onScrollLimit(int inputDeviceId, int source, int axis, boolean isStart) {
        throw new RuntimeException("stub");
    }
}
