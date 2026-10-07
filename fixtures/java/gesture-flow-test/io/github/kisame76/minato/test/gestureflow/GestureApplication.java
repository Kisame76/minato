package io.github.kisame76.minato.test.gestureflow;

import android.app.Application;
import android.util.Log;

/**
 * The test (ADR-0039 step b3.5b2-1b): Second says it is ready for its tap once it has the focus and First's gesture has
 * ended, whichever comes last, so that the log's order never hangs on when Android's focus comes.
 */
public class GestureApplication extends Application {
    static final String TAG = "gesture-flow-test";

    private static boolean sFirstEnded;
    private static boolean sSecondFocused;
    private static boolean sReady;

    static void log(String line) {
        Log.i(TAG, line);
    }

    static void firstEnded() {
        sFirstEnded = true;
        readyIfBoth();
    }

    static void secondFocused() {
        sSecondFocused = true;
        readyIfBoth();
    }

    private static void readyIfBoth() {
        if (sFirstEnded && sSecondFocused && !sReady) {
            sReady = true;
            log("Second ready");
        }
    }
}
