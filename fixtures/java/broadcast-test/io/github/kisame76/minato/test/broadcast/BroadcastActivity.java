package io.github.kisame76.minato.test.broadcast;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.PatternMatcher;
import android.os.SystemClock;
import android.util.Base64;
import android.util.Log;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * ADR-0045 decision 5's test: an app's own broadcasts within its process, from the resumed activity, in parts that run
 * one after the other. IntentFilter's matching and Base64 come first (the step that takes them into framework.jar),
 * then the receivers: the export flags Android 14 requires, an unordered broadcast to two receivers, ordered ones by
 * priority with their results, goAsync on another thread and an abort, an ordered one nobody receives, a receiver on a
 * handler of its own, an unregistered receiver, one unregistered right after an ordered broadcast to it is sent, and
 * one that the activity Leak leaks at its destroy. Each part logs with the tag broadcast-test in an order the threads
 * do not change; a part that waits for a broadcast gives up after 5 s with a line that says so, so that every run
 * reaches "done". A failure's stack has the tag broadcast-test-device.
 */
public class BroadcastActivity extends Activity {
    static final String TAG = "broadcast-test";
    static final String PREFIX = "io.github.kisame76.minato.test.broadcast.";
    static final String ACTION = PREFIX + "PING";
    static final String CATEGORY = PREFIX + "CATEGORY";
    static final String UNORDERED = PREFIX + "UNORDERED";
    static final String ORDERED = PREFIX + "ORDERED";
    static final String NOBODY = PREFIX + "NOBODY";
    static final String THREADED = PREFIX + "THREADED";
    static final String GONE = PREFIX + "GONE";
    static final String SENT_GONE = PREFIX + "SENT_GONE";
    static final String LEAK = PREFIX + "LEAK";
    static final long TIMEOUT_MS = 5000;

    /** The running test, which Leak tells of its destroy. */
    static BroadcastActivity sTest;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final List<Part> mParts = new ArrayList<>();
    private boolean mStarted;
    /** The running part's number, which a late answer of a part before it cannot end. */
    private int mPart;
    private Runnable mTimeout;

    private interface Part {
        void run(int part);
    }

    /** A receiver that hands what it gets to its callback, under a name for the log. */
    static final class Receiver extends BroadcastReceiver {
        interface Callback {
            void onReceive(Receiver receiver, Intent intent);
        }

        final String mName;
        private final Callback mCallback;

        Receiver(String name, Callback callback) {
            mName = name;
            mCallback = callback;
        }

        @Override
        public void onReceive(Context context, Intent intent) {
            mCallback.onReceive(this, intent);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!mStarted) {
            mStarted = true;
            sTest = this;
            mParts.addAll(Arrays.asList(this::filters, this::base64, this::flags, this::unordered, this::ordered,
                    this::nobody, this::threaded, this::unregistered, this::unregisteredWhileSent, this::leak));
            mHandler.post(this::next);
        }
    }

    /** Runs the next part, or ends the test. */
    private void next() {
        if (mTimeout != null) {
            mHandler.removeCallbacks(mTimeout);
            mTimeout = null;
        }
        if (mParts.isEmpty()) {
            Log.i(TAG, "done");
            return;
        }
        int part = ++mPart;
        try {
            mParts.remove(0).run(part);
        } catch (RuntimeException | LinkageError e) {
            Log.i(TAG, "failed: " + describe(e));
            Log.i(TAG + "-device", Log.getStackTraceString(e));
            end(part);
        }
    }

    /** Ends the part `part`, once. */
    private void end(int part) {
        if (part == mPart) {
            mPart = -1;
            mHandler.post(this::next);
        }
    }

    /** Ends the part `part` after the timeout with a line that names what it waited for. */
    private void waitFor(int part, String what) {
        mTimeout = () -> {
            Log.i(TAG, what + ": timeout");
            end(part);
        };
        mHandler.postDelayed(mTimeout, TIMEOUT_MS);
    }

    /** IntentFilter's matching of actions, categories and data, and its MIME types. */
    private void filters(int part) {
        IntentFilter filter = new IntentFilter(ACTION);
        filter.addCategory(CATEGORY);
        filter.addDataScheme("https");
        filter.addDataAuthority("example.org", null);
        filter.addDataPath("/packs", PatternMatcher.PATTERN_PREFIX);
        filter.setPriority(5);
        Log.i(TAG, "filter: actions " + filter.countActions() + ", has " + filter.hasAction(ACTION) + ", category "
                + filter.hasCategory(CATEGORY) + ", scheme " + filter.getDataScheme(0) + ", priority "
                + filter.getPriority());
        Uri uri = Uri.parse("https://example.org/packs/1");
        List<String> categories = Collections.singletonList(CATEGORY);
        Log.i(TAG, "match: all " + match(filter.match(ACTION, null, "https", uri, set(categories), TAG))
                + ", other action " + match(filter.match(PREFIX + "OTHER", null, "https", uri, set(categories), TAG))
                + ", no category " + match(filter.match(ACTION, null, "https", uri, null, TAG))
                + ", other category " + match(filter.match(ACTION, null, "https", uri,
                        set(Collections.singletonList(PREFIX + "ELSE")), TAG))
                + ", other host " + match(filter.match(ACTION, null, "https",
                        Uri.parse("https://example.com/packs/1"), set(categories), TAG))
                + ", other path " + match(filter.match(ACTION, null, "https", Uri.parse("https://example.org/other"),
                        set(categories), TAG))
                + ", no data " + match(filter.match(ACTION, null, null, null, set(categories), TAG)));
        Log.i(TAG, "match intent: " + match(filter.match(getContentResolver(),
                new Intent(ACTION, uri).addCategory(CATEGORY), false, TAG)));
        try {
            IntentFilter typed = IntentFilter.create(ACTION, "image/*");
            Log.i(TAG, "typed: png " + match(typed.match(ACTION, "image/png", null, null, null, TAG)) + ", text "
                    + match(typed.match(ACTION, "text/plain", null, null, null, TAG)));
            IntentFilter.create(ACTION, "bad");
            Log.i(TAG, "bad type: no exception");
        } catch (RuntimeException e) {
            Log.i(TAG, "bad type: " + e.getClass().getName() + ", cause "
                    + (e.getCause() == null ? null : e.getCause().getClass().getName()));
        }
        end(part);
    }

    /** Base64 in its default, URL-safe and wrapped forms. */
    private void base64(int part) {
        Log.i(TAG, "base64: " + Base64.encodeToString("Minato broadcast-test".getBytes(StandardCharsets.UTF_8),
                Base64.NO_WRAP));
        byte[] raw = {(byte) 0xfb, (byte) 0xff, 0x00, 0x10};
        String url = Base64.encodeToString(raw, Base64.URL_SAFE | Base64.NO_PADDING | Base64.NO_WRAP);
        Log.i(TAG, "base64 url: " + url + ", back " + Arrays.toString(Base64.decode(url, Base64.URL_SAFE)));
        Log.i(TAG, "base64 wrapped: " + Base64.encodeToString(new byte[60], Base64.DEFAULT).replace("\n", "|"));
        end(part);
    }

    /** Android 14's export flags at registerReceiver, for an app whose target SDK is 35. */
    private void flags(int part) {
        Receiver receiver = new Receiver("flags", (r, intent) -> { });
        IntentFilter custom = new IntentFilter(ACTION);
        register("no flag", receiver, custom, 0);
        register("both flags", receiver, custom, RECEIVER_EXPORTED | RECEIVER_NOT_EXPORTED);
        register("not exported", receiver, custom, RECEIVER_NOT_EXPORTED);
        register("protected only, no flag", receiver, new IntentFilter(Intent.ACTION_SCREEN_OFF), 0);
        IntentFilter mixed = new IntentFilter(Intent.ACTION_SCREEN_OFF);
        mixed.addAction(ACTION);
        register("protected and own, no flag", receiver, mixed, 0);
        try {
            Log.i(TAG, "null receiver, no flag: sticky " + (registerReceiver(null, custom) != null));
        } catch (RuntimeException e) {
            Log.i(TAG, "null receiver, no flag: " + describe(e));
        }
        try {
            unregisterReceiver(new Receiver("unknown", (r, intent) -> { }));
            Log.i(TAG, "unregister unknown: no exception");
        } catch (IllegalArgumentException e) {
            Log.i(TAG, "unregister unknown: " + e.getClass().getName() + ", \"Receiver not registered\" "
                    + e.getMessage().startsWith("Receiver not registered"));
        }
        end(part);
    }

    /** Registers and at once unregisters `receiver`, and logs what registerReceiver answered. */
    private void register(String what, Receiver receiver, IntentFilter filter, int flags) {
        try {
            Intent sticky = registerReceiver(receiver, filter, flags);
            unregisterReceiver(receiver);
            Log.i(TAG, what + ": registered, sticky " + (sticky != null));
        } catch (RuntimeException e) {
            Log.i(TAG, what + ": " + describe(e));
        }
    }

    /**
     * An unordered broadcast to two receivers: it returns before either receives, each gets its own copy of the
     * intent, and an abort does not stop the other.
     */
    private void unordered(int part) {
        List<String> got = new ArrayList<>();
        List<Receiver> receivers = new ArrayList<>();
        Receiver.Callback callback = (receiver, intent) -> {
            String line = receiver.mName + ": ordered " + receiver.isOrderedBroadcast() + ", sticky "
                    + receiver.isInitialStickyBroadcast() + ", another's change " + intent.hasExtra("changed")
                    + ", value " + intent.getIntExtra("value", -1) + ", main thread "
                    + (Looper.myLooper() == Looper.getMainLooper());
            intent.putExtra("changed", true);
            receiver.abortBroadcast();
            got.add(line + ", abort taken " + receiver.getAbortBroadcast());
            if (got.size() == receivers.size()) {
                Collections.sort(got);
                for (String received : got) {
                    Log.i(TAG, "unordered " + received);
                }
                unregister(receivers);
                end(part);
            }
        };
        receivers.add(new Receiver("A", callback));
        receivers.add(new Receiver("B", callback));
        for (Receiver receiver : receivers) {
            registerReceiver(receiver, new IntentFilter(UNORDERED), RECEIVER_NOT_EXPORTED);
        }
        waitFor(part, "unordered");
        sendBroadcast(new Intent(UNORDERED).setPackage(getPackageName()).putExtra("value", 1));
        Log.i(TAG, "unordered: sent, received before it returned " + got.size());
    }

    /**
     * Two ordered broadcasts to four receivers registered out of their priorities' order, P10 with goAsync finishing
     * on a thread of its own: the first runs through, the second P0a aborts. Each receiver adds to the result code,
     * data and extras; the final receiver gets them, also after the abort.
     */
    private void ordered(int part) {
        List<Receiver> receivers = new ArrayList<>();
        Receiver.Callback callback = (receiver, intent) -> {
            Bundle extras = receiver.getResultExtras(true);
            Log.i(TAG, "ordered " + receiver.mName + ": code " + receiver.getResultCode() + ", data "
                    + receiver.getResultData() + ", k " + extras.getInt("k") + ", ordered "
                    + receiver.isOrderedBroadcast());
            if (receiver.mName.equals("P10")) {
                BroadcastReceiver.PendingResult result = receiver.goAsync();
                new Thread(() -> {
                    SystemClock.sleep(100);
                    Bundle later = result.getResultExtras(true);
                    later.putInt("k", later.getInt("k") + 1);
                    result.setResultCode(result.getResultCode() + 1);
                    result.setResultData(result.getResultData() + "/P10");
                    Log.i(TAG, "ordered P10: finishes on its own thread");
                    result.finish();
                }, "broadcast-test-async").start();
                return;
            }
            extras.putInt("k", extras.getInt("k") + 1);
            receiver.setResultCode(receiver.getResultCode() + 1);
            receiver.setResultData(receiver.getResultData() + "/" + receiver.mName);
            if (receiver.mName.equals("P0a") && intent.getBooleanExtra("abort", false)) {
                receiver.abortBroadcast();
                Log.i(TAG, "ordered P0a: aborts");
            }
        };
        // Out of the priorities' order; P0a before P0b, which have the same priority.
        int[] priorities = {-10, 0, 10, 0};
        String[] names = {"Pm10", "P0a", "P10", "P0b"};
        for (int i = 0; i < names.length; i++) {
            Receiver receiver = new Receiver(names[i], callback);
            IntentFilter filter = new IntentFilter(ORDERED);
            filter.setPriority(priorities[i]);
            registerReceiver(receiver, filter, RECEIVER_NOT_EXPORTED);
            receivers.add(receiver);
        }
        waitFor(part, "ordered");
        sendOrdered(new Intent(ORDERED), "ordered", () -> sendOrdered(new Intent(ORDERED).putExtra("abort", true),
                "ordered with an abort", () -> {
                    unregister(receivers);
                    end(part);
                }));
    }

    /** An ordered broadcast with an initial result, whose final receiver logs what it got and then runs `then`. */
    private void sendOrdered(Intent intent, String what, Runnable then) {
        Bundle initial = new Bundle();
        initial.putInt("k", 0);
        sendOrderedBroadcast(intent.setPackage(getPackageName()), null, new Receiver("final", (receiver, got) -> {
            Bundle extras = receiver.getResultExtras(false);
            Log.i(TAG, what + " final: code " + receiver.getResultCode() + ", data " + receiver.getResultData()
                    + ", k " + (extras == null ? null : extras.getInt("k")) + ", main thread "
                    + (Looper.myLooper() == Looper.getMainLooper()));
            then.run();
        }), null, 1, "d", initial);
    }

    /** An ordered broadcast no receiver matches: its final receiver still gets the initial result. */
    private void nobody(int part) {
        waitFor(part, "nobody");
        sendOrderedBroadcast(new Intent(NOBODY).setPackage(getPackageName()), null,
                new Receiver("final", (receiver, intent) -> {
                    Log.i(TAG, "nobody final: code " + receiver.getResultCode() + ", data " + receiver.getResultData()
                            + ", extras " + receiver.getResultExtras(false));
                    end(part);
                }), null, 7, "empty", null);
    }

    /** A receiver on a handler of its own gets the broadcast on that handler's thread. */
    private void threaded(int part) {
        HandlerThread thread = new HandlerThread("broadcast-test-handler");
        thread.start();
        Receiver receiver = new Receiver("threaded", (r, intent) ->
                Log.i(TAG, "threaded: on " + Thread.currentThread().getName()));
        registerReceiver(receiver, new IntentFilter(THREADED), null, new Handler(thread.getLooper()),
                RECEIVER_NOT_EXPORTED);
        waitFor(part, "threaded");
        sendOrdered(new Intent(THREADED), "threaded", () -> {
            unregisterReceiver(receiver);
            thread.quitSafely();
            end(part);
        });
    }

    /** An unregistered receiver gets nothing. */
    private void unregistered(int part) {
        boolean[] received = {false};
        Receiver receiver = new Receiver("gone", (r, intent) -> received[0] = true);
        registerReceiver(receiver, new IntentFilter(GONE), RECEIVER_NOT_EXPORTED);
        unregisterReceiver(receiver);
        waitFor(part, "unregistered");
        sendOrdered(new Intent(GONE), "unregistered", () -> {
            Log.i(TAG, "unregistered: received " + received[0]);
            end(part);
        });
    }

    /**
     * An ordered broadcast to R1, R2 and R3 by priority whose first receiver is unregistered right after the send, in
     * the same message: R1 never receives, and R2, R3 and then the final receiver each get the result as the one
     * before left it, since the broadcast goes on only when the process acknowledges R1's delivery.
     */
    private void unregisteredWhileSent(int part) {
        List<Receiver> receivers = new ArrayList<>();
        Receiver.Callback callback = (receiver, intent) -> {
            Log.i(TAG, "unregistered while sent " + receiver.mName + ": data " + receiver.getResultData());
            receiver.setResultData(receiver.mName);
        };
        int[] priorities = {10, 0, -10};
        for (int i = 0; i < priorities.length; i++) {
            Receiver receiver = new Receiver("R" + (i + 1), callback);
            IntentFilter filter = new IntentFilter(SENT_GONE);
            filter.setPriority(priorities[i]);
            registerReceiver(receiver, filter, RECEIVER_NOT_EXPORTED);
            receivers.add(receiver);
        }
        waitFor(part, "unregistered while sent");
        sendOrdered(new Intent(SENT_GONE), "unregistered while sent", () -> {
            unregister(receivers.subList(1, receivers.size()));
            end(part);
        });
        unregisterReceiver(receivers.get(0));
    }

    private int mLeakPart;

    /** A receiver that Leak registers and does not unregister goes at Leak's destroy. */
    private void leak(int part) {
        mLeakPart = part;
        waitFor(part, "leak");
        startActivity(new Intent(this, LeakActivity.class));
    }

    /** Leak's onDestroy: a broadcast after the destroy's cleanup, which the main thread runs after it. */
    void leakDestroyed() {
        int part = mLeakPart;
        mHandler.postDelayed(() -> sendOrdered(new Intent(LEAK), "leak", () -> {
            Log.i(TAG, "leak: the leaked receiver received " + LeakActivity.sReceived + " after the destroy");
            end(part);
        }), 500);
    }

    private void unregister(List<Receiver> receivers) {
        for (Receiver receiver : receivers) {
            unregisterReceiver(receiver);
        }
    }

    private static java.util.Set<String> set(List<String> items) {
        return new java.util.HashSet<>(items);
    }

    /** IntentFilter.match's answer: a match's category and adjustment in hex, a mismatch's negative reason. */
    private static String match(int result) {
        return result < 0 ? String.valueOf(result) : "0x" + Integer.toHexString(result);
    }

    static String describe(Throwable t) {
        return t.getClass().getName() + ": " + t.getMessage();
    }
}
