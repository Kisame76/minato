package com.android.minato.os;

/**
 * Who asked for the process's own SIGKILL (Process.killProcess(myPid()) and every other sendSignal to itself), as one
 * line of the session log, which a user's crash report may carry: an app that ends itself leaves no exception and no
 * fault, so this line is all that tells a quit through the engine from one through a store library. The caller's
 * frames are named by family only, never by an app's own classes: the platform's frames (java., javax., sun.,
 * android., com.android., dalvik., libcore.), which Minato provides, keep their class and method; frames of a known
 * library family (an engine, Google Play's libraries, Kotlin's, the common ad, attribution and network libraries) are
 * that family's name; every other frame is "app".
 * A run of frames of one family folds into one entry with its count. Uses no android.* class, so the host's JVM tests
 * it (tools/framework-host-check.sh).
 */
public final class SelfKill {
    /** Families checked before the platform's prefixes, since Play's licensing interface lives in com.android. */
    private static final String[][] FAMILIES = {
        {"com.unity3d.", "unity"},
        {"org.godotengine.", "godot"},
        {"com.google.android.gms.", "play-services"},
        {"com.google.android.play.", "play-core"},
        {"com.google.android.vending.licensing.", "play-licensing"},
        {"com.android.vending.", "play-licensing"},
        {"com.google.firebase.", "firebase"},
        {"kotlin.", "kotlin"},
        {"kotlinx.", "kotlin"},
        {"com.facebook.", "facebook"},
        {"com.applovin.", "applovin"},
        {"com.ironsource.", "ironsource"},
        {"com.vungle.", "vungle"},
        {"com.adjust.", "adjust"},
        {"com.appsflyer.", "appsflyer"},
        {"com.chartboost.", "chartboost"},
        {"com.inmobi.", "inmobi"},
        {"com.mbridge.", "mintegral"},
        {"com.bytedance.", "pangle"},
        {"com.fyber.", "fyber"},
        {"io.bidmachine.", "bidmachine"},
        {"com.startapp.", "startapp"},
        {"okhttp3.", "okhttp"},
        {"com.squareup.", "square"},
    };

    private static final String[] PLATFORM = {
        "java.", "javax.", "sun.", "android.", "com.android.", "dalvik.", "libcore.",
    };

    /** At most this many entries after folding; the rest are counted in a last "+N more". */
    static final int MAX_ENTRIES = 16;

    private SelfKill() {
    }

    /**
     * The line for a self-kill seen from the frames of Thread.getStackTrace() on the killing thread: the frames of the
     * stack walk itself (Thread, VMStack), of android.os.Process and of this class are left out, so the first entry is
     * the one that called into Process.
     */
    public static String describe(StackTraceElement[] frames, boolean mainThread) {
        StringBuilder line = new StringBuilder("self-kill (SIGKILL) on the ")
                .append(mainThread ? "main" : "another").append(" thread; callers:");
        String last = null;
        int run = 0;
        int entries = 0;
        int dropped = 0;
        boolean started = false;
        for (StackTraceElement frame : frames) {
            String type = frame.getClassName();
            if (!started) {
                if (type.equals("java.lang.Thread") || type.equals("dalvik.system.VMStack")
                        || type.equals("android.os.Process") || type.equals(SelfKill.class.getName())) {
                    continue;
                }
                started = true;
            }
            String name = nameOf(frame);
            if (name.equals(last)) {
                run++;
                continue;
            }
            if (last != null) {
                if (entries < MAX_ENTRIES) {
                    append(line, last, run);
                    entries++;
                } else {
                    dropped += run;
                }
            }
            last = name;
            run = 1;
        }
        if (last != null) {
            if (entries < MAX_ENTRIES) {
                append(line, last, run);
            } else {
                dropped += run;
            }
        }
        if (!started) {
            line.append(" none");
        }
        if (dropped > 0) {
            line.append(" < +").append(dropped).append(" more");
        }
        return line.toString();
    }

    /** A frame's name under the mask: the platform's class and method, a library family, or "app". */
    static String nameOf(StackTraceElement frame) {
        String type = frame.getClassName();
        for (String[] family : FAMILIES) {
            if (type.startsWith(family[0])) {
                return family[1];
            }
        }
        for (String prefix : PLATFORM) {
            if (type.startsWith(prefix)) {
                return type + "." + frame.getMethodName();
            }
        }
        return "app";
    }

    private static void append(StringBuilder line, String name, int run) {
        line.append(line.charAt(line.length() - 1) == ':' ? " " : " < ").append(name);
        if (run > 1) {
            line.append(" x").append(run);
        }
    }
}
