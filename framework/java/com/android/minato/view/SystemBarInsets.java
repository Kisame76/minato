package com.android.minato.view;

/**
 * The display's system bars as the window frames of floating windows see them (ADR-0041 decision 8, finding 3): the
 * status bar's height at the top and the navigation bar's at the bottom, in pixels. Android lays a dialog or a floating
 * activity out inside the bars, so its frame centers between them; with none (the default, and the apps' own insets
 * stay none) the frame centers in the whole display, which shows only where the bars differ in height.
 * This is the seam where the host's values plug in: the checks give them in the environment, MINATO_BAR_INSETS as
 * "<top>,<bottom>" (tools/check-display.sh, from the AVDs' views-test rows); the iPhone's come from the app's
 * GuestInsets through set() once the session passes them.
 */
public final class SystemBarInsets {
    private static int sTop;
    private static int sBottom;
    private static boolean sRead;

    private SystemBarInsets() {
    }

    /** The host's values, which win over the environment. */
    public static synchronized void set(int top, int bottom) {
        sTop = Math.max(top, 0);
        sBottom = Math.max(bottom, 0);
        sRead = true;
    }

    public static synchronized int top() {
        read();
        return sTop;
    }

    public static synchronized int bottom() {
        read();
        return sBottom;
    }

    private static void read() {
        if (sRead) {
            return;
        }
        sRead = true;
        final String value = System.getenv("MINATO_BAR_INSETS");
        if (value == null) {
            return;
        }
        final String[] parts = value.split(",");
        try {
            if (parts.length == 2) {
                sTop = Math.max(Integer.parseInt(parts[0].trim()), 0);
                sBottom = Math.max(Integer.parseInt(parts[1].trim()), 0);
            }
        } catch (NumberFormatException e) {
            sTop = 0;
            sBottom = 0;
        }
    }
}
