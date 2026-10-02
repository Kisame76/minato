package android.hardware.display;

import android.content.res.Resources;
import android.view.Display;
import android.view.DisplayAdjustments;
import android.view.DisplayInfo;

/**
 * Compile-only stub (ADR-0039 b3.4), never in framework.jar: ResourcesManager asks it for a display's metrics and
 * adjusted display, which a Resources built from metrics and a configuration never does. Step c's Context answers
 * the display.
 */
public final class DisplayManagerGlobal {
    private DisplayManagerGlobal() {}

    public static DisplayManagerGlobal getInstance() {
        throw new RuntimeException("stub");
    }

    public DisplayInfo getDisplayInfo(int displayId) {
        throw new RuntimeException("stub");
    }

    public Display getCompatibleDisplay(int displayId, DisplayAdjustments daj) {
        throw new RuntimeException("stub");
    }

    public Display getCompatibleDisplay(int displayId, Resources resources) {
        throw new RuntimeException("stub");
    }
}
