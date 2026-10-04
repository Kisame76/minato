// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// Surface's hardware canvas (lockHardwareCanvas), which only hwui's GPU pipeline gives (M4 step 10).

import android.view.Surface;

class HardwareRenderer {
    /** @hide */
    public void setSurface(Surface surface, boolean discardBuffer) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public float setColorMode(int colorMode) {
        throw new RuntimeException("stub");
    }
}
