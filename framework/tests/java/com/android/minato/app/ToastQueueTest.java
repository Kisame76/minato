package com.android.minato.app;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * ADR-0041 (b4) on the host's JVM (tools/framework-host-check.sh): the toast queue against a fake host, as
 * Android 15's NotificationManagerService keeps it. The host's calls go into one log, so their order is checked too:
 * one toast shows at a time, for 2 s or 3.5 s, a text toast 333 ms more and one after a text toast 250 ms more; a
 * package queues at most five; a token queued again only changes its duration; a cancel hides and the next shows; an
 * app that is not in the foreground, whose notifications are off, queues none, and a toast with a view of its own that
 * comes up while the app is not in the foreground is blocked.
 */
public final class ToastQueueTest {
    private static final String PKG = "io.github.kisame76.app";
    private final List<String> mLog = new ArrayList<>();
    private boolean mForeground = true;
    private boolean mShowFails;
    private ToastQueue.Toast mScheduled;
    private final ToastQueue mQueue = new ToastQueue(new ToastQueue.Host() {
        @Override
        public boolean show(ToastQueue.Toast toast) {
            mLog.add("show " + toast.token);
            return !mShowFails;
        }

        @Override
        public void hide(ToastQueue.Toast toast) {
            mLog.add("hide " + toast.token);
        }

        @Override
        public void schedule(ToastQueue.Toast toast, long delay) {
            mLog.add("schedule " + toast.token + " " + delay);
            mScheduled = toast;
        }

        @Override
        public void unschedule(ToastQueue.Toast toast) {
            if (mScheduled == toast) {
                mScheduled = null;
            }
        }

        @Override
        public boolean foreground() {
            return mForeground;
        }

        @Override
        public void log(String line) {
            mLog.add("log " + line);
        }
    });

    private boolean text(String token, int duration) {
        return mQueue.enqueue(PKG, token, true, duration);
    }

    private boolean custom(String token, int duration) {
        return mQueue.enqueue(PKG, token, false, duration);
    }

    /** The scheduled duration's end, as the host's timer calls it. */
    private void end() {
        ToastQueue.Toast toast = mScheduled;
        check(toast != null, "a toast's end is scheduled");
        mScheduled = null;
        mQueue.durationReached(toast);
    }

    private void expect(String... lines) {
        check(mLog.equals(List.of(lines)), "log " + mLog + ", expected " + List.of(lines));
        mLog.clear();
    }

    void testATextToastShowsAtOnceForTwoSecondsAndItsInAnimation() {
        check(text("a", ToastQueue.LENGTH_SHORT), "queued");
        expect("show a", "schedule a 2333");
        end();
        expect("hide a");
    }

    void testALongTextToastShowsThreeAndAHalfSeconds() {
        text("a", ToastQueue.LENGTH_LONG);
        expect("show a", "schedule a 3833");
    }

    void testAToastWithAViewHasNoInAnimation() {
        custom("c", ToastQueue.LENGTH_SHORT);
        expect("show c", "schedule c 2000");
    }

    void testTheNextShowsAfterTheEndWithThePreviousOnesOutAnimation() {
        text("a", ToastQueue.LENGTH_SHORT);
        text("b", ToastQueue.LENGTH_SHORT);
        custom("c", ToastQueue.LENGTH_LONG);
        expect("show a", "schedule a 2333");
        end();
        expect("hide a", "show b", "schedule b 2583");
        end();
        expect("hide b", "show c", "schedule c 3750");
        end();
        expect("hide c");
    }

    void testAPackageQueuesAtMostFive() {
        for (int i = 1; i <= 5; i++) {
            check(text("t" + i, ToastQueue.LENGTH_SHORT), "toast " + i + " queued");
        }
        mLog.clear();
        check(!text("t6", ToastQueue.LENGTH_SHORT), "the sixth is refused");
        expect("log Package has already queued 5 toasts. Not showing more. Toast pkg=" + PKG);
    }

    void testATokenQueuedAgainChangesOnlyItsDuration() {
        text("a", ToastQueue.LENGTH_SHORT);
        text("b", ToastQueue.LENGTH_SHORT);
        mLog.clear();
        check(text("b", ToastQueue.LENGTH_LONG), "queued again");
        expect();
        end();
        expect("hide a", "show b", "schedule b 4083");
    }

    void testACancelHidesTheCurrentAndShowsTheNext() {
        text("a", ToastQueue.LENGTH_SHORT);
        text("b", ToastQueue.LENGTH_SHORT);
        mLog.clear();
        mQueue.cancel(PKG, "a");
        expect("hide a", "show b", "schedule b 2583");
    }

    void testACancelOfAWaitingToastRemovesIt() {
        text("a", ToastQueue.LENGTH_SHORT);
        text("b", ToastQueue.LENGTH_SHORT);
        text("c", ToastQueue.LENGTH_SHORT);
        mLog.clear();
        mQueue.cancel(PKG, "b");
        expect("hide b");
        end();
        expect("hide a", "show c", "schedule c 2583");
    }

    void testACancelOfAnUnknownTokenIsLogged() {
        mQueue.cancel(PKG, "x");
        expect("log Toast already cancelled. pkg=" + PKG + " token=x");
    }

    void testAnAppNotInTheForegroundQueuesNone() {
        mForeground = false;
        check(!text("a", ToastQueue.LENGTH_SHORT), "refused");
        expect("log Suppressing toast from package " + PKG + " by user request.");
    }

    void testAToastWithAViewIsBlockedWhenTheAppLeftTheForeground() {
        text("a", ToastQueue.LENGTH_SHORT);
        custom("c", ToastQueue.LENGTH_SHORT);
        text("b", ToastQueue.LENGTH_SHORT);
        mLog.clear();
        mForeground = false;
        end();
        expect("hide a", "log Blocking custom toast from package " + PKG
                + " due to package not in the foreground at the time of showing the toast", "show b",
                "schedule b 2583");
    }

    void testAToastThatCannotShowGoesAndTheNextShows() {
        mShowFails = true;
        text("a", ToastQueue.LENGTH_SHORT);
        expect("show a");
        mShowFails = false;
        text("b", ToastQueue.LENGTH_SHORT);
        expect("show b", "schedule b 2333");
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : ToastQueueTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new ToastQueueTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("ToastQueueTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("ToastQueueTest: " + run + " of " + run + " fine");
    }
}
