/*
 * Our GraphicsEnvironment, under AOSP's name (ADR-0039 step c2c3): the one call an app process makes, from
 * Activity.performStart. AOSP's shows a dialog when ANGLE is in use and the developer option for it is on, which it
 * reads from Settings.Global (GraphicsEnvironment.java:639-651). Minato's GLES is always ANGLE (ADR-0029) and has no
 * such option, so it shows nothing, as Android does with the option off. AOSP's other members choose the GLES driver
 * at the bind, which Minato's EGL front does instead.
 */
package android.os;

import android.content.Context;

/** @hide */
public final class GraphicsEnvironment {
    private static final GraphicsEnvironment sInstance = new GraphicsEnvironment();

    private GraphicsEnvironment() {
    }

    public static GraphicsEnvironment getInstance() {
        return sInstance;
    }

    /** As AOSP's with the developer option off: no dialog. */
    public void showAngleInUseDialogBox(Context context) {
    }
}
