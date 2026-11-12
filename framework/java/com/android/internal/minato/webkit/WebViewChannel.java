package com.android.internal.minato.webkit;

import android.os.Handler;
import android.os.Looper;

import org.json.JSONException;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicLong;

/**
 * One WebView's link to the host (ADR-0056 step b2): APKRT_REQUEST_WEBVIEW's MAKE, PLACE, REMOVE, CALL and ANSWER, and
 * a pump thread that reads the host's records from the channel MAKE opened and hands each, in order, to the main
 * Looper. A CALL is a JSON object; one whose JSON passes a record's limit (1 MiB) goes in parts under one request_id,
 * its large field cut, {"part": n, "last": true|false}, which the host joins. A RESOURCE, the
 * host's ask for a page's request of the app's scheme (step c1), goes to the provider on the pump itself, which
 * answers it from a thread of its own, as Chromium asks shouldInterceptRequest off the UI thread.
 */
final class WebViewChannel {
    /** APKRT_WEBVIEW_*'s ops (guest_interface.h). */
    static final int MAKE = 1;
    static final int PLACE = 2;
    static final int REMOVE = 3;
    static final int CALL = 4;
    static final int ANSWER = 5;

    /** apkrt_webview_record_type (guest_interface.h). */
    static final int REC_CREATED = 1;
    static final int REC_PLACED = 2;
    static final int REC_DESTROYED = 3;
    static final int REC_NAVIGATION = 4;
    static final int REC_JS_RESULT = 5;
    static final int REC_JS_BRIDGE = 6;
    static final int REC_RESOURCE = 7;
    static final int REC_PERMISSION = 8;
    static final int REC_ERROR = 9;
    static final int REC_JS_DIALOG = 10;
    static final int REC_CONSOLE = 11;
    static final int REC_PROGRESS = 12;
    static final int REC_TITLE = 13;
    static final int REC_COOKIES = 14;

    /** APKRT_WEBVIEW_RECORD_MAX: the most bytes of one CALL. */
    static final int RECORD_MAX = 1 << 20;
    /** The characters of a cut field in one part: JSON's worst escape, six bytes a character, stays under 1 MiB. */
    static final int PART_CHARS = 128 * 1024;

    /**
     * What the provider does with each record, on the main Looper, but RESOURCE's and JS_BRIDGE's, on the pump
     * (COOKIES: sCookies).
     */
    interface Records {
        void onRecord(int type, long requestId, JSONObject payload);

        /** A RESOURCE record, on the pump: the provider answers it, without blocking the pump. */
        void onResource(long requestId, JSONObject request);

        /** A JS_BRIDGE record, on the pump: the provider answers it, without blocking the pump or the main Looper. */
        default void onJavaCall(long requestId, JSONObject call) {}

        /** The host closed the channel, or it failed: no more records come. */
        void onChannelEnd();
    }

    private static final AtomicLong sTokens = new AtomicLong();
    /** Where every channel's COOKIES records go, on its pump: the process's CookieManager (ADR-0056 c3). */
    static volatile java.util.function.Consumer<JSONObject> sCookies;

    private final long mToken = sTokens.incrementAndGet();
    private final AtomicLong mRequestIds = new AtomicLong();
    private final Handler mMain = new Handler(Looper.getMainLooper());
    private int mFd = -1;

    /**
     * MAKE: opens the view's channel and starts its pump. Returns 0, or the negative errno of the refusal, such as
     * -EOPNOTSUPP where the host has no WebView.
     */
    int make(Records records) {
        int fd = nativeRequest(MAKE, mToken, 0, 0, null, null);
        if (fd < 0) {
            return fd;
        }
        mFd = fd;
        Thread pump = new Thread(() -> pump(fd, records), "MinatoWebView pump " + mToken);
        pump.setDaemon(true);
        pump.start();
        return 0;
    }

    boolean isOpen() {
        return mFd >= 0;
    }

    /** A new request id, for a CALL whose answer comes as a record with it. */
    long nextRequestId() {
        return mRequestIds.incrementAndGet();
    }

    /**
     * CALL: the JSON object `call`, in parts when it passes a record's limit, cutting its string field `cut`. 0, or the
     * negative errno of the first refused part.
     */
    int call(long requestId, JSONObject call, String cut) {
        byte[] whole = call.toString().getBytes(StandardCharsets.UTF_8);
        if (whole.length <= RECORD_MAX || cut == null) {
            return nativeRequest(CALL, mToken, requestId, 0, null, whole);
        }
        String value = call.optString(cut);
        try {
            JSONObject first = new JSONObject(call.toString());
            int parts = (value.length() + PART_CHARS - 1) / PART_CHARS;
            for (int part = 0; part < parts; part++) {
                JSONObject piece = part == 0 ? first : new JSONObject().put("call", call.getString("call"));
                int end = Math.min(value.length(), (part + 1) * PART_CHARS);
                piece.put("part", part).put("last", part == parts - 1)
                        .put(cut, value.substring(part * PART_CHARS, end));
                int result = nativeRequest(CALL, mToken, requestId, 0, null,
                        piece.toString().getBytes(StandardCharsets.UTF_8));
                if (result < 0) {
                    return result;
                }
            }
            return 0;
        } catch (JSONException e) {
            return -22; // EINVAL: a call the provider made, so this does not happen
        }
    }

    /** ANSWER: the guest's answer to the host's record `requestId`. */
    int answer(long requestId, JSONObject answer) {
        return nativeRequest(ANSWER, mToken, requestId, 0, null,
                answer.toString().getBytes(StandardCharsets.UTF_8));
    }

    /** ANSWER with raw bytes, `length` of them from `offset`: a piece of a resource's body (its end has none). */
    int answerBytes(long requestId, byte[] bytes, int offset, int length) {
        byte[] piece = offset == 0 && length == bytes.length ? bytes
                : Arrays.copyOfRange(bytes, offset, offset + length);
        return nativeRequest(ANSWER, mToken, requestId, 0, null, piece);
    }

    /** PLACE: the frame and clip in the window's pixels, as left, top, right, bottom of each. */
    int place(long window, int[] frameAndClip) {
        return nativeRequest(PLACE, mToken, 0, window, frameAndClip, null);
    }

    /** REMOVE: the host closes the channel, so the pump meets its end. */
    void remove() {
        if (mFd >= 0) {
            mFd = -1;
            nativeRequest(REMOVE, mToken, 0, 0, null, null);
        }
    }

    private void pump(int fd, Records records) {
        long[] meta = new long[3];
        while (true) {
            byte[] payload = nativeNext(fd, meta);
            if (meta[0] != 1) {
                break;
            }
            int type = (int) meta[1];
            long requestId = meta[2];
            JSONObject object;
            try {
                object = payload.length == 0 ? new JSONObject()
                        : new JSONObject(new String(payload, StandardCharsets.UTF_8));
            } catch (JSONException e) {
                object = new JSONObject();
            }
            JSONObject record = object;
            java.util.function.Consumer<JSONObject> cookies = sCookies;
            if (type == REC_RESOURCE) {
                records.onResource(requestId, record);
            } else if (type == REC_JS_BRIDGE) {
                records.onJavaCall(requestId, record);
            } else if (type == REC_COOKIES) {
                // On the pump, before the records after it reach the main Looper: a getCookie in onPageFinished, or
                // the main thread's first getCookie waiting for the list, sees it.
                if (cookies != null) {
                    cookies.accept(record);
                }
            } else {
                mMain.post(() -> records.onRecord(type, requestId, record));
            }
        }
        nativeClose(fd);
        mMain.post(records::onChannelEnd);
    }

    private static native int nativeRequest(int op, long token, long requestId, long window, int[] frame,
            byte[] payload);

    private static native byte[] nativeNext(int fd, long[] meta);

    private static native void nativeClose(int fd);
}
