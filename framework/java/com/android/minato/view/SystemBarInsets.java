package com.android.minato.view;

/**
 * The display's system bars as the window frames of floating windows see them (ADR-0041 decision 8, finding 3): the
 * status bar's height at the top and the navigation bar's at the bottom, in pixels. Android lays a dialog or a floating
 * activity out inside the bars, so its frame centers between them; with none (the default, and the apps' own insets
 * stay none) the frame centers in the whole display, which shows only where the bars differ in height.
 * This is the seam where the host's values plug in: the checks give them in the environment, MINATO_BAR_INSETS as
 * "<top>,<bottom>" (tools/check-display.sh, from the AVDs' views-test rows); the iPhone's come the same way, the
 * session appends them to the guest's environment at every start from the app's GuestInsets. The runtime takes the
 * variable out of the environment at start, so an app does not see it, and ActivityThread.barInsets() has its value,
 * which ActivityThread passes to initial() at its start.
 * set() is the seam for a later update, as at a turn of the display during a session.
 */
public final class SystemBarInsets {
    private static int sTop;
    private static int sBottom;
    private static boolean sRead;

    private SystemBarInsets() {
    }

    /**
     * The values the host passed in the environment, as "<top>,<bottom>" or null for none, which ActivityThread hands
     * over at its start (this package has no android.* in it, so that it compiles on the host alone). A set() before
     * or after wins.
     */
    public static synchronized void initial(String value) {
        if (sRead || value == null) {
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

    /** The host's values, which win over the environment. */
    public static synchronized void set(int top, int bottom) {
        sTop = Math.max(top, 0);
        sBottom = Math.max(bottom, 0);
        sRead = true;
    }

    public static synchronized int top() {
        return sTop;
    }

    public static synchronized int bottom() {
        return sBottom;
    }
}
