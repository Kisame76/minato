package com.android.minato.os;

import java.lang.reflect.Method;

/**
 * The self-kill line (SelfKill) on the host's JVM (tools/framework-host-check.sh): the platform's frames keep their
 * class and method, library families are named, an app's own classes are only "app", runs fold with a count, the stack
 * walk's and Process's own frames are left out, and a long stack ends in "+N more".
 */
public final class SelfKillTest {
    private static StackTraceElement frame(String type, String method) {
        return new StackTraceElement(type, method, null, -1);
    }

    public void testFamiliesAndThePlatformAreNamedAndAppClassesAreNot() {
        StackTraceElement[] frames = {
            frame("dalvik.system.VMStack", "getThreadStackTrace"),
            frame("java.lang.Thread", "getStackTrace"),
            frame("android.os.Process", "sendSignal"),
            frame("android.os.Process", "killProcess"),
            frame("com.example.secret.Quit", "now"),
            frame("com.example.secret.Quit", "later"),
            frame("com.unity3d.player.UnityPlayer", "kill"),
            frame("com.google.android.vending.licensing.LicenseChecker", "finish"),
            frame("com.android.vending.licensing.ILicenseResultListener$Stub", "onTransact"),
            frame("android.os.Handler", "dispatchMessage"),
            frame("android.os.Looper", "loop"),
        };
        String line = SelfKill.describe(frames, true);
        check(line.equals("self-kill (SIGKILL) on the main thread; callers: app x2 < unity < play-licensing x2"
                + " < android.os.Handler.dispatchMessage < android.os.Looper.loop"), line);
        check(!line.contains("example") && !line.contains("secret"), "an app's class name in " + line);
    }

    public void testAnotherThreadAndNoCaller() {
        StackTraceElement[] frames = {
            frame("java.lang.Thread", "getStackTrace"),
            frame("android.os.Process", "killProcess"),
        };
        String line = SelfKill.describe(frames, false);
        check(line.equals("self-kill (SIGKILL) on the another thread; callers: none"), line);
    }

    public void testALongStackEndsWithACount() {
        StackTraceElement[] frames = new StackTraceElement[SelfKill.MAX_ENTRIES + 2];
        for (int index = 0; index < frames.length; index++) {
            frames[index] = index % 2 == 0 ? frame("org.example.A" + index, "run")
                    : frame("android.view.View" + index, "draw");
        }
        String line = SelfKill.describe(frames, true);
        check(line.endsWith(" < +2 more"), line);
        check(line.split(" < ").length == SelfKill.MAX_ENTRIES + 1, line);
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : SelfKillTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new SelfKillTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("SelfKillTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("SelfKillTest: " + run + " of " + run + " fine");
    }
}
