package io.github.kisame76.minato.test.looper;

import android.app.Application;
import android.content.Context;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.MessageQueue;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import java.io.FileDescriptor;
import java.io.InterruptedIOException;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * ADR-0039 step c1's test of MessageQueue's six natives, which framework/native gives on M3's ALooper (decision 10):
 * from Application.attachBaseContext, a HandlerThread's Looper (nativeInit) runs a posted message, which wakes it
 * (nativeWake) from its poll (nativePollOnce), and a delayed one; hands a pipe's input to a listener
 * (nativeSetFileDescriptorEvents); and quits (nativeDestroy). nativeIsPolling stays untested: only the hidden
 * MessageQueue.isPolling reaches it, which an app cannot call on Android. It logs with the tag looper-test and ends
 * with "done"; tools/android-reference.sh compares it with Android 15's log. Not from the static
 * initializer: the messages' lambdas are static methods of this class, which the Looper's thread could not run
 * before the initializer ends.
 */
public class LooperApplication extends Application {
    private static final String TAG = "looper-test";

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            report();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    static void report() throws InterruptedException, ErrnoException, InterruptedIOException {
        HandlerThread thread = new HandlerThread("looper-test-thread");
        thread.start();
        Looper looper = thread.getLooper();
        Log.i(TAG, "looper " + (looper != null) + ", on its thread " + (looper.getThread() == thread));
        Handler handler = new Handler(looper);

        // Let the thread block in its poll first, so that the message must wake it.
        SystemClock.sleep(200);
        CountDownLatch posted = new CountDownLatch(1);
        handler.post(() -> {
            Log.i(TAG, "message on " + Thread.currentThread().getName());
            posted.countDown();
        });
        Log.i(TAG, "message ran " + posted.await(5, TimeUnit.SECONDS));

        CountDownLatch delayed = new CountDownLatch(1);
        long[] waited = new long[1];
        long start = SystemClock.uptimeMillis();
        handler.postDelayed(() -> {
            waited[0] = SystemClock.uptimeMillis() - start;
            delayed.countDown();
        }, 300);
        Log.i(TAG, "delayed message ran " + delayed.await(5, TimeUnit.SECONDS) + ", after 300 ms or more "
                + (waited[0] >= 300));

        SystemClock.sleep(200);
        MessageQueue queue = looper.getQueue();
        Log.i(TAG, "idle " + queue.isIdle());

        FileDescriptor[] pipe = Os.pipe();
        CountDownLatch read = new CountDownLatch(1);
        String[] got = new String[1];
        queue.addOnFileDescriptorEventListener(pipe[0], MessageQueue.OnFileDescriptorEventListener.EVENT_INPUT,
                (fd, events) -> {
                    byte[] buffer = new byte[16];
                    try {
                        int length = Os.read(fd, buffer, 0, buffer.length);
                        got[0] = "events " + events + ", read \""
                                + new String(buffer, 0, length, StandardCharsets.UTF_8) + "\" on "
                                + Thread.currentThread().getName();
                    } catch (ErrnoException | InterruptedIOException e) {
                        got[0] = "events " + events + ", " + e;
                    }
                    read.countDown();
                    // No more events: the queue stops watching the descriptor.
                    return 0;
                });
        Os.write(pipe[1], "ping".getBytes(StandardCharsets.UTF_8), 0, 4);
        Log.i(TAG, "descriptor listener ran " + read.await(5, TimeUnit.SECONDS) + ": " + got[0]);
        Os.close(pipe[0]);
        Os.close(pipe[1]);

        Log.i(TAG, "quitSafely " + thread.quitSafely());
        thread.join(5000);
        Log.i(TAG, "thread ended " + !thread.isAlive());
    }
}
