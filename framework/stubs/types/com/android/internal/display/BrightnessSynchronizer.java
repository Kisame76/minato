package com.android.internal.display;

/**
 * Compile-only stub (ADR-0041 a)), never in framework.jar: DisplayInfo.equals compares its brightness values through it,
 * which no path of an app runs yet: gap, with the display listeners of ADR-0041 c2c3's window.
 */
public final class BrightnessSynchronizer {
    public static boolean floatEquals(float a, float b) {
        throw new RuntimeException("stub");
    }
}
