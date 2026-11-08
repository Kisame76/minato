package io.github.kisame76.minato.test.enginestart;

import android.app.Activity;
import android.view.Display;

/**
 * engine-start-test's class of the APK that its native code finds through the activity's class loader, as a frame
 * pacer finds its display manager: it hands the default display's refresh periods, one per supported mode, to a
 * native method that the native code registers.
 */
public final class PacingProbe {
    private PacingProbe() {
    }

    private static native void nSetSupportedRefreshPeriods(long[] periods);

    public static void start(Activity activity) {
        Display display = activity.getWindowManager().getDefaultDisplay();
        Display.Mode[] modes = display.getSupportedModes();
        long[] periods = new long[modes.length];
        for (int index = 0; index < modes.length; index++) {
            periods[index] = (long) (1e9 / modes[index].getRefreshRate());
        }
        nSetSupportedRefreshPeriods(periods);
    }
}
