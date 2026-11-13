package com.android.minato.app;

import java.util.ArrayList;

/**
 * The process's toast queue (ADR-0041 (b4)), in plain Java so that the host's JVM tests it, as Android 15's
 * NotificationManagerService keeps its queue (enqueueToast, showNextToastLocked, cancelToastLocked,
 * scheduleDurationReachedLocked at android-15.0.0_r1):
 * <ul>
 *   <li>a toast is queued while the app is in the foreground, as an app without POST_NOTIFICATIONS, whose notifications
 *       are off, may queue one only then; a package has at most five queued; a token queued again changes only its
 *       duration;
 *   <li>the first in the queue shows, one at a time, for 2 s (LENGTH_SHORT) or 3.5 s (LENGTH_LONG), a text toast 333 ms
 *       more for its in animation, and one after a text toast 250 ms more for that one's out animation; a toast with a
 *       view of its own that comes up while the app is not in the foreground is blocked, and one that cannot show
 *       goes, the next one showing instead;
 *   <li>at its duration's end or its cancel a toast is hidden and leaves the queue, and the next one shows.
 * </ul>
 * Android's rate limit counts only toasts of an app not in the foreground, which ours never queues, and the
 * accessibility's recommended timeout is the delay itself without accessibility services. The host's calls come on the
 * thread of the queue's calls, one at a time.
 */
public final class ToastQueue {
    /** Toast.LENGTH_SHORT and LENGTH_LONG. */
    public static final int LENGTH_SHORT = 0;
    public static final int LENGTH_LONG = 1;
    /** NotificationManagerService's SHORT_DELAY, LONG_DELAY and MAX_PACKAGE_TOASTS. */
    static final int SHORT_DELAY = 2000;
    static final int LONG_DELAY = 3500;
    static final int MAX_PACKAGE_TOASTS = 5;
    /** scheduleDurationReachedLocked's animations: a text toast's in, the text toast's before it out. */
    static final int TEXT_IN_ANIMATION = 333;
    static final int TEXT_OUT_ANIMATION = 250;

    /** What the queue asks of the system around it. */
    public interface Host {
        /** Shows the toast; false when it cannot. */
        boolean show(Toast toast);

        /** Hides the toast, which may not have shown: then it does nothing. */
        void hide(Toast toast);

        /** Calls durationReached(toast) in `delay` ms, in place of an earlier call for it. */
        void schedule(Toast toast, long delay);

        /** Takes back the toast's scheduled call. */
        void unschedule(Toast toast);

        /** Whether the app has a resumed activity, as ActivityTaskManager.hasResumedActivity answers. */
        boolean foreground();

        /** NotificationManagerService's log line. */
        void log(String line);
    }

    /** A queued toast, as ToastRecord. */
    public static final class Toast {
        public final String pkg;
        public final Object token;
        /** A text toast, which the system draws, or a toast with a view of its own. */
        public final boolean text;
        int mDuration;

        Toast(String pkg, Object token, boolean text, int duration) {
            this.pkg = pkg;
            this.token = token;
            this.text = text;
            mDuration = duration;
        }

        public int duration() {
            return mDuration;
        }
    }

    private final Host mHost;
    private final ArrayList<Toast> mQueue = new ArrayList<>();
    /** mIsCurrentToastShown: the first one shows. */
    private boolean mCurrentShown;

    public ToastQueue(Host host) {
        mHost = host;
    }

    /** enqueueTextToast (`text`) or enqueueToast: whether the toast was queued. */
    public boolean enqueue(String pkg, Object token, boolean text, int duration) {
        if (!mHost.foreground()) {
            mHost.log("Suppressing toast from package " + pkg + " by user request.");
            return false;
        }
        final int index = indexOf(pkg, token);
        if (index >= 0) {
            mQueue.get(index).mDuration = duration;
            return true;
        }
        int count = 0;
        for (Toast toast : mQueue) {
            if (toast.pkg.equals(pkg)) {
                count++;
            }
        }
        if (count >= MAX_PACKAGE_TOASTS) {
            mHost.log("Package has already queued " + count + " toasts. Not showing more. Toast pkg=" + pkg);
            return false;
        }
        mQueue.add(new Toast(pkg, token, text, duration));
        if (mQueue.size() == 1) {
            showNext(false);
        }
        return true;
    }

    /** cancelToast: the toast hides and leaves the queue. */
    public void cancel(String pkg, Object token) {
        final int index = indexOf(pkg, token);
        if (index >= 0) {
            cancelAt(index);
        } else {
            mHost.log("Toast already cancelled. pkg=" + pkg + " token=" + token);
        }
    }

    /** handleDurationReached: the scheduled end of the toast that shows. */
    public void durationReached(Toast toast) {
        final int index = indexOf(toast.pkg, toast.token);
        if (index >= 0) {
            cancelAt(index);
        }
    }

    private int indexOf(String pkg, Object token) {
        for (int i = 0; i < mQueue.size(); i++) {
            final Toast toast = mQueue.get(i);
            if (toast.pkg.equals(pkg) && toast.token == token) {
                return i;
            }
        }
        return -1;
    }

    private void showNext(boolean lastWasText) {
        if (mCurrentShown) {
            return;
        }
        while (!mQueue.isEmpty()) {
            final Toast toast = mQueue.get(0);
            final boolean shown;
            if (!toast.text && !mHost.foreground()) {
                mHost.log("Blocking custom toast from package " + toast.pkg
                        + " due to package not in the foreground at the time of showing the toast");
                shown = false;
            } else {
                shown = mHost.show(toast);
            }
            if (shown) {
                long delay = toast.mDuration == LENGTH_LONG ? LONG_DELAY : SHORT_DELAY;
                if (lastWasText) {
                    delay += TEXT_OUT_ANIMATION;
                }
                if (toast.text) {
                    delay += TEXT_IN_ANIMATION;
                }
                mHost.unschedule(toast);
                mHost.schedule(toast, delay);
                mCurrentShown = true;
                return;
            }
            mQueue.remove(0);
        }
    }

    private void cancelAt(int index) {
        final Toast toast = mQueue.get(index);
        mHost.unschedule(toast);
        mHost.hide(toast);
        if (index == 0) {
            mCurrentShown = false;
        }
        mQueue.remove(index);
        if (!mQueue.isEmpty()) {
            showNext(toast.text);
        }
    }
}
