package com.android.internal.minato.webkit;

import android.content.Context;
import android.content.Intent;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Picture;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.net.http.SslCertificate;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.print.PrintDocumentAdapter;
import android.util.Log;
import android.view.DragEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewRootImpl;
import android.view.ViewStructure;
import android.view.ViewTreeObserver;
import android.view.WindowInsets;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.util.Base64;
import android.webkit.JavascriptInterface;
import android.webkit.MimeTypeMap;
import android.webkit.ConsoleMessage;
import android.webkit.DownloadListener;
import android.webkit.JsPromptResult;
import android.webkit.JsResult;
import android.webkit.ValueCallback;
import android.webkit.WebBackForwardList;
import android.webkit.WebChromeClient;
import android.webkit.WebHistoryItem;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.webkit.WebViewProvider;
import android.webkit.WebViewRenderProcess;
import android.webkit.WebViewRenderProcessClient;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * One WebView's provider (ADR-0056 steps b1 and b2): what Chromium's WebViewChromium is to AOSP's facade, with the
 * page in the host. It keeps what the app sets (settings, clients, listeners, the background color) and sends the
 * loads, scripts and navigation to the host over its WebViewChannel, which it opens at the first load or at its attach,
 * whichever comes first: a CALL "make" first, with the package's data store, the scheme and the settings, then one
 * per call, with "settings" again before a load when they changed (the graphics review of b2). URLs of the app's assets
 * and resources (file:///android_asset/, file:///android_res/) go to the host as minato-asset://asset/ and
 * minato-asset://res/, its other file:// URLs, which name the guest's files, as minato-asset://file/, and they come
 * back as the app gave them, in every URL the app sees. The host's records come back on
 * the main Looper: they drive the clients and a mirror of the page (URL, original URL, title, progress, history), from
 * which the getters answer. At each pre-draw while attached it places the view in its window's pixels (PLACE).
 * Without a host (none has a WebView yet, MAKE answers -EOPNOTSUPP) it answers as a WebView that has loaded nothing
 * does, as in b1, and draws as Chromium's does on Android 15 (webview-start-test on the AVD): opaque black before the
 * app asks for any content, then its background color, white unless the app sets one; a load or a script is logged
 * once per process, without its URL, and a script's callback gets "null" so that an app waiting for it goes on. With a
 * host its area is cleared, as SurfaceView's hole, for the host's view. AOSP's facade checks the UI thread before
 * each call.
 */
@SuppressWarnings("deprecation") // The interface's deprecated members, which Chromium's implements too.
final class WebViewProviderImpl implements WebViewProvider, WebViewProvider.ViewDelegate,
        WebViewProvider.ScrollDelegate {
    /** The one tag of the provider's notes, so that a report shows them without page data. */
    static final String TAG = "MinatoWebView";
    private static boolean sNoContentLogged;

    private final WebView mWebView;
    private final WebView.PrivateAccess mPrivate;
    private final WebSettings mSettings;
    private volatile WebViewClient mClient = new WebViewClient(); // read by the resource threads too
    private WebChromeClient mChromeClient;
    private WebViewRenderProcessClient mRenderProcessClient;
    private int mBackgroundColor = Color.WHITE;
    /** The app has asked for content (a load or a script): from then on the page's area shows the background. */
    private boolean mContentRequested;
    private final Paint mBackgroundPaint = new Paint();
    private boolean mPaused;
    private int mRendererPriority = WebView.RENDERER_PRIORITY_IMPORTANT;
    private boolean mRendererWaived;

    /** The host's view: not asked yet, open, or absent (no host has a WebView; b1's answers). */
    private static final int HOST_UNKNOWN = 0;
    private static final int HOST_OPEN = 1;
    private static final int HOST_ABSENT = 2;
    private int mHost = HOST_UNKNOWN;
    private final WebViewChannel mChannel = new WebViewChannel();
    /** The settings the host has, as last sent. */
    private JSONObject mSentSettings;
    /** The page's mirror, from the host's records. */
    private String mUrl;
    private String mOriginalUrl;
    private String mTitle = "";
    private int mProgress = 100;
    private final List<HistoryItem> mHistory = new ArrayList<>();
    private int mHistoryIndex = -1;
    /** evaluateJavascript's callbacks, by their CALL's request id. */
    private final Map<Long, ValueCallback<String>> mScripts = new HashMap<>();
    /** The frame and clip last placed, and the window they were placed in. */
    private int[] mPlaced;
    private long mPlacedWindow;
    /** The touch region last sent, as left, top, right, bottom quadruples in the window's pixels. */
    private int[] mTouchRegion;
    private final ViewTreeObserver.OnPreDrawListener mPlacer = () -> {
        place();
        return true;
    };

    WebViewProviderImpl(WebView webView, WebView.PrivateAccess privateAccess) {
        mWebView = webView;
        mPrivate = privateAccess;
        mSettings = new WebSettingsImpl(webView.getContext());
    }

    /** The app asked for content: without a host, the area turns to the page's background. */
    private void requested() {
        if (!mContentRequested) {
            mContentRequested = true;
            mWebView.invalidate();
        }
    }

    /** Without a host the page does not come: noted once per process, without what was asked. */
    private static synchronized void noContent() {
        if (!sNoContentLogged) {
            sNoContentLogged = true;
            Log.w(TAG, "web content not available yet: the WebView stays empty (ADR-0056 step c)");
        }
    }

    // The host

    /** The host's view, opened at the first need: MAKE, then the CALL "make". Whether the host has one. */
    private boolean hostOpen() {
        if (mHost == HOST_UNKNOWN) {
            if (mChannel.make(mRecords) < 0) {
                mHost = HOST_ABSENT;
            } else {
                mHost = HOST_OPEN;
                mSentSettings = settings();
                String dataStore = UUID.nameUUIDFromBytes(("minato-webview:" + mWebView.getContext().getPackageName())
                        .getBytes(StandardCharsets.UTF_8)).toString();
                try {
                    send(new JSONObject().put("call", "make").put("dataStore", dataStore)
                            .put("schemes", new JSONArray().put(SCHEME)).put("settings", mSentSettings), null, 0);
                } catch (JSONException e) {
                    throw new IllegalStateException(e);
                }
            }
        }
        return mHost == HOST_OPEN;
    }

    /** What the host's view needs of the settings; a change applies from the next load (the graphics review). */
    private JSONObject settings() {
        try {
            return new JSONObject().put("userAgent", mSettings.getUserAgentString())
                    .put("javaScript", mSettings.getJavaScriptEnabled())
                    .put("domStorage", mSettings.getDomStorageEnabled())
                    .put("mediaNeedsGesture", mSettings.getMediaPlaybackRequiresUserGesture())
                    .put("blockNetworkLoads", mSettings.getBlockNetworkLoads())
                    .put("textZoom", mSettings.getTextZoom())
                    .put("wideViewPort", mSettings.getUseWideViewPort());
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    /** A CALL to the host; a refusal is noted as b1's missing content is. */
    private void send(JSONObject call, String cut, long requestId) {
        if (mChannel.call(requestId, call, cut) < 0) {
            noContent();
        }
    }

    /** A load: "settings" first when they changed since the host last had them, then the load itself. */
    private void load(JSONObject call, String cut) {
        requested();
        if (!hostOpen()) {
            noContent();
            return;
        }
        JSONObject now = settings();
        if (!now.toString().equals(mSentSettings.toString())) {
            mSentSettings = now;
            try {
                send(new JSONObject().put("call", "settings").put("settings", now), null, 0);
            } catch (JSONException e) {
                throw new IllegalStateException(e);
            }
        }
        send(call, cut, 0);
    }

    /** A call that only the host can carry out (navigation, the cache), dropped without one. */
    private void command(String name, String key, Object value) {
        if (!hostOpen()) {
            return;
        }
        try {
            JSONObject call = new JSONObject().put("call", name);
            if (key != null) {
                call.put(key, value);
            }
            send(call, null, 0);
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    private static JSONObject call(String name) {
        try {
            return new JSONObject().put("call", name);
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    /** The scheme of the app's assets and resources in the host. */
    static final String SCHEME = "minato-asset";
    private static final String[][] URL_PREFIXES = {
        {"file:///android_asset/", SCHEME + "://asset/"},
        {"file:///android_res/", SCHEME + "://res/"},
        {"file:///", SCHEME + "://file/"},
    };

    /** A URL of the app as the host sees it. */
    static String toHost(String url) {
        if (url != null) {
            for (String[] prefix : URL_PREFIXES) {
                if (url.startsWith(prefix[0])) {
                    return prefix[1] + url.substring(prefix[0].length());
                }
            }
        }
        return url;
    }

    /** A URL of the host as the app gave it. */
    static String toApp(String url) {
        if (url != null) {
            for (String[] prefix : URL_PREFIXES) {
                if (url.startsWith(prefix[1])) {
                    return prefix[0] + url.substring(prefix[1].length());
                }
            }
        }
        return url;
    }

    /** A URL field of a record, as the app gave it; null when absent. */
    private static String url(JSONObject record, String name) {
        return record.isNull(name) ? null : toApp(record.optString(name));
    }

    // The host's records, on the main Looper

    private final WebViewChannel.Records mRecords = new WebViewChannel.Records() {
        @Override
        public void onRecord(int type, long requestId, JSONObject record) {
            switch (type) {
                case WebViewChannel.REC_NAVIGATION:
                    navigation(record);
                    break;
                case WebViewChannel.REC_PROGRESS:
                    mProgress = record.optInt("progress", 100);
                    if (mChromeClient != null) {
                        mChromeClient.onProgressChanged(mWebView, mProgress);
                    }
                    break;
                case WebViewChannel.REC_TITLE:
                    mTitle = record.optString("title", "");
                    if (mHistoryIndex >= 0) {
                        mHistory.get(mHistoryIndex).mTitle = mTitle;
                    }
                    if (mChromeClient != null) {
                        mChromeClient.onReceivedTitle(mWebView, mTitle);
                    }
                    break;
                case WebViewChannel.REC_JS_RESULT: {
                    ValueCallback<String> callback = mScripts.remove(requestId);
                    if (callback != null) {
                        callback.onReceiveValue(record.isNull("json") ? null : record.optString("json"));
                    }
                    break;
                }
                case WebViewChannel.REC_JS_DIALOG:
                    dialog(requestId, record);
                    break;
                case WebViewChannel.REC_ERROR:
                    error(record);
                    break;
                case WebViewChannel.REC_CONSOLE:
                    console(record);
                    break;
                default:
                    break; // CREATED, PLACED and DESTROYED change nothing the app sees
            }
        }

        @Override
        public void onResource(long requestId, JSONObject request) {
            RESOURCES.execute(() -> serve(requestId, request));
        }

        @Override
        public void onJavaCall(long requestId, JSONObject call) {
            BRIDGE.execute(() -> {
                try {
                    mChannel.answer(requestId, invoke(call));
                } catch (JSONException e) {
                    throw new IllegalStateException(e);
                }
            });
        }

        @Override
        public void onChannelEnd() {
            for (ValueCallback<String> callback : mScripts.values()) {
                callback.onReceiveValue(null);
            }
            mScripts.clear();
        }
    };

    /** The thread of the app's objects, which the pages call (addJavascriptInterface), as Chromium's JavaBridge. */
    private static final ExecutorService BRIDGE = Executors.newSingleThreadExecutor(runnable -> {
        Thread thread = new Thread(runnable, "JavaBridge");
        thread.setDaemon(true);
        return thread;
    });
    /** The app's objects for the pages, by name. */
    private final Map<String, Object> mInterfaces = new HashMap<>();

    /** The threads that answer the host's RESOURCE records, as Chromium's IO threads ask shouldInterceptRequest. */
    private static final ExecutorService RESOURCES = Executors.newCachedThreadPool(runnable -> {
        Thread thread = new Thread(runnable, "MinatoWebView resources");
        thread.setDaemon(true);
        return thread;
    });

    /** The most bytes of one answer of a body: an ANSWER's limit is 1 MiB. */
    private static final int BODY_PIECE = 256 * 1024;

    /**
     * A request of the page for the app's scheme (step c1): the app's shouldInterceptRequest first, as on Android, then
     * the asset or resource itself. Answers the head ({"status", "reason", "mimeType", "encoding", "headers"}; 404 for
     * nothing, with "denied" for a file while file access is off), then the body in pieces, then an empty answer; an
     * end without a head fails the request at the host.
     */
    private void serve(long requestId, JSONObject record) {
        String url = url(record, "url");
        Map<String, String> headers = new HashMap<>();
        JSONObject given = record.optJSONObject("headers");
        if (given != null) {
            for (Iterator<String> keys = given.keys(); keys.hasNext();) {
                String key = keys.next();
                headers.put(key, given.optString(key));
            }
        }
        Request request = new Request(url, record.optBoolean("mainFrame"), record.optString("method", "GET"), headers);
        // An exception of the app's escapes on this thread and ends the app, as on Chromium's IO thread.
        WebResourceResponse response = mClient.shouldInterceptRequest(mWebView, request);
        if (response == null) {
            response = local(url);
        }
        InputStream body = response != null ? response.getData() : null;
        try {
            JSONObject head = new JSONObject();
            if (response == null) {
                head.put("status", 404);
                if (deniedFile(url)) {
                    head.put("denied", true);
                }
            } else {
                // A response without a status (its three-argument constructor's) is 200 OK, as Chromium takes it.
                int status = response.getStatusCode();
                head.put("status", status == 0 ? 200 : status)
                        .put("reason", status == 0 ? "OK" : response.getReasonPhrase())
                        .put("mimeType", response.getMimeType()).put("encoding", response.getEncoding());
                if (response.getResponseHeaders() != null) {
                    head.put("headers", new JSONObject(response.getResponseHeaders()));
                }
            }
            mChannel.answer(requestId, head);
            if (body != null) {
                byte[] piece = new byte[BODY_PIECE];
                int filled = 0;
                for (int count; (count = body.read(piece, filled, piece.length - filled)) > 0;) {
                    filled += count;
                    if (filled == piece.length) {
                        mChannel.answerBytes(requestId, piece, 0, filled);
                        filled = 0;
                    }
                }
                if (filled > 0) {
                    mChannel.answerBytes(requestId, piece, 0, filled);
                }
            }
        } catch (IOException | JSONException e) {
            Log.w(TAG, "a resource's body failed", e);
        } finally {
            if (body != null) {
                try {
                    body.close();
                } catch (IOException e) {
                    // closed anyway
                }
            }
            mChannel.answerBytes(requestId, new byte[0], 0, 0);
        }
    }

    /**
     * An asset (file:///android_asset/<path>) or a resource (file:///android_res/<type>/<name>.<extension>) of the
     * app, as Chromium's AndroidProtocolHandler opens them, or another file of the guest, which Chromium reads only
     * while getAllowFileAccess holds (HTML5 app wrappers serve their pages from the app's files), with the MIME type of
     * its extension; null for none.
     */
    private WebResourceResponse local(String url) {
        if (url == null) {
            return null;
        }
        String path = Uri.parse(url).getPath();
        if (path == null) {
            return null;
        }
        String extension = MimeTypeMap.getFileExtensionFromUrl(url);
        String mime = extension.isEmpty() ? null : MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension);
        try {
            if (url.startsWith(URL_PREFIXES[0][0])) {
                InputStream stream = mWebView.getContext().getAssets().open(
                        path.substring("/android_asset/".length()));
                return new WebResourceResponse(mime, null, stream);
            }
            if (url.startsWith(URL_PREFIXES[1][0])) {
                String[] parts = path.substring("/android_res/".length()).split("/");
                if (parts.length != 2) {
                    return null;
                }
                String name = parts[1].contains(".") ? parts[1].substring(0, parts[1].lastIndexOf('.')) : parts[1];
                Context context = mWebView.getContext();
                int id = context.getResources().getIdentifier(name, parts[0], context.getPackageName());
                return id == 0 ? null : new WebResourceResponse(mime, null,
                        context.getResources().openRawResource(id));
            }
            if (url.startsWith(URL_PREFIXES[2][0]) && mSettings.getAllowFileAccess()) {
                return new WebResourceResponse(mime, null, new FileInputStream(path));
            }
        } catch (IOException | RuntimeException e) {
            return null;
        }
        return null;
    }

    /** A file of the guest, not an asset or a resource, while file access is off: Chromium's ERR_ACCESS_DENIED. */
    private boolean deniedFile(String url) {
        return url != null && url.startsWith(URL_PREFIXES[2][0]) && !url.startsWith(URL_PREFIXES[0][0])
                && !url.startsWith(URL_PREFIXES[1][0]) && !mSettings.getAllowFileAccess();
    }

    private void navigation(JSONObject record) {
        String phase = record.optString("phase");
        String url = url(record, "url");
        switch (phase) {
            case "started":
                mClient.onPageStarted(mWebView, url, null);
                break;
            case "committed":
                mUrl = url;
                mOriginalUrl = url(record, "originalUrl");
                JSONArray items = record.optJSONArray("history");
                if (items != null) {
                    mHistory.clear();
                    for (int i = 0; i < items.length(); i++) {
                        JSONObject item = items.optJSONObject(i);
                        mHistory.add(new HistoryItem(i, url(item, "url"), url(item, "originalUrl"),
                                item.optString("title", "")));
                    }
                    mHistoryIndex = record.optInt("index", mHistory.size() - 1);
                }
                mClient.doUpdateVisitedHistory(mWebView, url, record.optBoolean("reload"));
                break;
            case "finished":
                mClient.onPageFinished(mWebView, url);
                break;
            default:
                break;
        }
    }

    private void error(JSONObject record) {
        Request request = new Request(url(record, "url"), record.optBoolean("mainFrame"));
        if (record.optBoolean("http")) {
            mClient.onReceivedHttpError(mWebView, request, new WebResourceResponse(null, null,
                    record.optInt("status"), record.optString("reason", "Error"), Collections.emptyMap(), null));
        } else {
            mClient.onReceivedError(mWebView, request,
                    new Error(record.optInt("code"), record.optString("description")));
        }
    }

    /** A script's dialog, which the host waits for: the chrome client's answer, else a cancel, as no dialog shows. */
    private void dialog(long requestId, JSONObject record) {
        String kind = record.optString("kind");
        String url = url(record, "url");
        String message = record.optString("message");
        JsResult.ResultReceiver receiver = result -> {
            try {
                JSONObject answer = new JSONObject().put("result", result.getResult());
                if (result instanceof JsPromptResult) {
                    answer.put("value", ((JsPromptResult) result).getStringResult());
                }
                mChannel.answer(requestId, answer);
            } catch (JSONException e) {
                throw new IllegalStateException(e);
            }
        };
        boolean handled = false;
        JsResult result;
        if (kind.equals("prompt")) {
            JsPromptResult prompt = new JsPromptResult(receiver);
            result = prompt;
            handled = mChromeClient != null
                    && mChromeClient.onJsPrompt(mWebView, url, message, record.optString("default"), prompt);
        } else {
            result = new JsResult(receiver);
            if (mChromeClient != null) {
                switch (kind) {
                    case "alert":
                        handled = mChromeClient.onJsAlert(mWebView, url, message, result);
                        break;
                    case "confirm":
                        handled = mChromeClient.onJsConfirm(mWebView, url, message, result);
                        break;
                    case "beforeunload":
                        handled = mChromeClient.onJsBeforeUnload(mWebView, url, message, result);
                        break;
                    default:
                        break;
                }
            }
        }
        if (!handled) {
            result.cancel();
        }
    }

    /**
     * A page's console message: to the client, and, when there is none or it returns false, to the log as Chromium's
     * WebView logs it, at every level an info line of the tag chromium.
     */
    private void console(JSONObject record) {
        String message = record.optString("message");
        String source = url(record, "source");
        int line = record.optInt("line");
        if (mChromeClient == null || !mChromeClient.onConsoleMessage(new ConsoleMessage(message, source, line,
                level(record.optString("level"))))) {
            Log.i("chromium", "[INFO:CONSOLE(" + line + ")] \"" + message + "\", source: " + source
                    + " (" + line + ")");
        }
    }

    private static ConsoleMessage.MessageLevel level(String name) {
        switch (name) {
            case "tip": return ConsoleMessage.MessageLevel.TIP;
            case "warning": return ConsoleMessage.MessageLevel.WARNING;
            case "error": return ConsoleMessage.MessageLevel.ERROR;
            case "debug": return ConsoleMessage.MessageLevel.DEBUG;
            default: return ConsoleMessage.MessageLevel.LOG;
        }
    }

    /** PLACE: the view's frame and its visible part in its window's pixels, when they changed. */
    private void place() {
        if (mHost != HOST_OPEN) {
            return;
        }
        ViewRootImpl root = mWebView.getViewRootImpl();
        long window = root != null ? root.hostWindow() : 0;
        if (window == 0 || mWebView.getWidth() <= 0 || mWebView.getHeight() <= 0) {
            return;
        }
        int[] at = new int[2];
        mWebView.getLocationInWindow(at);
        Rect visible = new Rect();
        if (!mWebView.getLocalVisibleRect(visible)) {
            visible.setEmpty();
        } else {
            visible.offset(at[0] - mWebView.getScrollX(), at[1] - mWebView.getScrollY());
        }
        int[] frame = {at[0], at[1], at[0] + mWebView.getWidth(), at[1] + mWebView.getHeight(),
                visible.left, visible.top, visible.right, visible.bottom};
        if (window != mPlacedWindow || !java.util.Arrays.equals(frame, mPlaced)) {
            mPlacedWindow = window;
            mPlaced = frame;
            mChannel.place(window, frame);
        }
        int[] region = touchRegion(visible, root.getView());
        if (!java.util.Arrays.equals(region, mTouchRegion)) {
            mTouchRegion = region;
            try {
                JSONArray rects = new JSONArray();
                for (int i = 0; i < region.length; i += 4) {
                    rects.put(new JSONArray().put(region[i]).put(region[i + 1]).put(region[i + 2])
                            .put(region[i + 3]));
                }
                send(call("touchRegion").put("rects", rects), null, 0);
            } catch (JSONException e) {
                throw new IllegalStateException(e);
            }
        }
    }

    /**
     * The touch region (ADR-0056 decision 2), which the host's hit test gives the page: the WebView's visible part
     * less the visible views drawn after it that take touches as View.onTouchEvent does (clickable, long-clickable or
     * context-clickable; a view that is only focusable lets them through), its own children included, in the window's
     * tree order; the window's pixels, as left, top, right, bottom quadruples. Named limits: a child drawing order or
     * a z is not followed; views that take touches without being clickable are not cut out (a scrolling container's
     * onTouchEvent or onInterceptTouchEvent, a view with only an OnTouchListener); nor is a child outside a covering
     * view's bounds (clipChildren false), whose children are skipped.
     */
    private int[] touchRegion(Rect visible, View root) {
        List<Rect> region = new ArrayList<>();
        if (!visible.isEmpty()) {
            region.add(new Rect(visible));
        }
        boolean[] after = {false};
        coverAfter(root, region, after);
        int[] flat = new int[region.size() * 4];
        for (int i = 0; i < region.size(); i++) {
            Rect rect = region.get(i);
            flat[i * 4] = rect.left;
            flat[i * 4 + 1] = rect.top;
            flat[i * 4 + 2] = rect.right;
            flat[i * 4 + 3] = rect.bottom;
        }
        return flat;
    }

    /** Takes from `region` the views from the WebView on that take touches; `after` is set at the WebView. */
    private void coverAfter(View view, List<Rect> region, boolean[] after) {
        if (view == null || view.getVisibility() != View.VISIBLE) {
            return;
        }
        if (view == mWebView) {
            after[0] = true;
        } else if (after[0] && (view.isClickable() || view.isLongClickable() || view.isContextClickable())) {
            Rect covered = new Rect();
            if (view.getGlobalVisibleRect(covered)) {
                subtract(region, covered);
            }
            return;  // its children lie inside it
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                coverAfter(group.getChildAt(i), region, after);
            }
        }
    }

    /** `region` less `cut`: each rectangle it meets splits into the up to four bands around it. */
    static void subtract(List<Rect> region, Rect cut) {
        List<Rect> rest = new ArrayList<>();
        for (Rect rect : region) {
            if (!Rect.intersects(rect, cut)) {
                rest.add(rect);
                continue;
            }
            int top = Math.max(rect.top, cut.top);
            int bottom = Math.min(rect.bottom, cut.bottom);
            if (cut.top > rect.top) {
                rest.add(new Rect(rect.left, rect.top, rect.right, cut.top));
            }
            if (cut.bottom < rect.bottom) {
                rest.add(new Rect(rect.left, cut.bottom, rect.right, rect.bottom));
            }
            if (cut.left > rect.left) {
                rest.add(new Rect(rect.left, top, cut.left, bottom));
            }
            if (cut.right < rect.right) {
                rest.add(new Rect(cut.right, top, rect.right, bottom));
            }
        }
        region.clear();
        region.addAll(rest);
    }

    /** A history entry of the host's, as its last record gave it. */
    private static final class HistoryItem extends WebHistoryItem {
        private final int mId;
        private final String mUrl;
        private final String mOriginalUrl;
        String mTitle;

        HistoryItem(int id, String url, String originalUrl, String title) {
            mId = id;
            mUrl = url;
            mOriginalUrl = originalUrl;
            mTitle = title;
        }

        @Override public int getId() { return mId; }
        @Override public String getUrl() { return mUrl; }
        @Override public String getOriginalUrl() { return mOriginalUrl; }
        @Override public String getTitle() { return mTitle; }
        @Override public Bitmap getFavicon() { return null; }
        @Override protected HistoryItem clone() { return new HistoryItem(mId, mUrl, mOriginalUrl, mTitle); }
    }

    /** A copy of the history, as copyBackForwardList gives it; empty before the first page, as Chromium's. */
    @SuppressWarnings("serial")
    private static final class History extends WebBackForwardList {
        private final List<HistoryItem> mItems;
        private final int mIndex;

        History(List<HistoryItem> items, int index) {
            mItems = new ArrayList<>();
            for (HistoryItem item : items) {
                mItems.add(item.clone());
            }
            mIndex = index;
        }

        @Override
        public WebHistoryItem getCurrentItem() {
            return mIndex >= 0 && mIndex < mItems.size() ? mItems.get(mIndex) : null;
        }

        @Override public int getCurrentIndex() { return mIndex; }

        @Override
        public WebHistoryItem getItemAtIndex(int index) {
            return index >= 0 && index < mItems.size() ? mItems.get(index) : null;
        }

        @Override public int getSize() { return mItems.size(); }
        @Override protected WebBackForwardList clone() { return new History(mItems, mIndex); }
    }

    /** The request of an error record: a GET without a gesture, as the host reports a failed load. */
    private static final class Request implements WebResourceRequest {
        private final Uri mUrl;
        private final boolean mMainFrame;
        private final String mMethod;
        private final Map<String, String> mHeaders;

        Request(String url, boolean mainFrame) {
            this(url, mainFrame, "GET", Collections.emptyMap());
        }

        Request(String url, boolean mainFrame, String method, Map<String, String> headers) {
            mUrl = url != null ? Uri.parse(url) : Uri.EMPTY;
            mMainFrame = mainFrame;
            mMethod = method;
            mHeaders = headers;
        }

        @Override public Uri getUrl() { return mUrl; }
        @Override public boolean isForMainFrame() { return mMainFrame; }
        @Override public boolean isRedirect() { return false; }
        @Override public boolean hasGesture() { return false; }
        @Override public String getMethod() { return mMethod; }
        @Override public Map<String, String> getRequestHeaders() { return mHeaders; }
    }

    /** A load's error: Chromium's code and its net:: description. */
    private static final class Error extends WebResourceError {
        private final int mCode;
        private final String mDescription;

        Error(int code, String description) {
            mCode = code;
            mDescription = description;
        }

        @Override public int getErrorCode() { return mCode; }
        @Override public CharSequence getDescription() { return mDescription; }
    }

    // WebViewProvider

    @Override
    public void init(Map<String, Object> javaScriptInterfaces, boolean privateBrowsing) {
        if (privateBrowsing && mWebView.getContext().getApplicationInfo().targetSdkVersion
                >= Build.VERSION_CODES.KITKAT) {
            throw new IllegalArgumentException("Private browsing is not supported in WebView.");
        }
    }

    @Override public void setHorizontalScrollbarOverlay(boolean overlay) {}
    @Override public void setVerticalScrollbarOverlay(boolean overlay) {}
    @Override public boolean overlayHorizontalScrollbar() { return true; }
    @Override public boolean overlayVerticalScrollbar() { return false; }
    @Override public int getVisibleTitleHeight() { return 0; }
    @Override public SslCertificate getCertificate() { return null; }
    @Override public void setCertificate(SslCertificate certificate) {}
    @Override public void savePassword(String host, String username, String password) {}
    @Override public void setHttpAuthUsernamePassword(String host, String realm, String username, String password) {}
    @Override public String[] getHttpAuthUsernamePassword(String host, String realm) { return null; }
    @Override public void destroy() { mChannel.remove(); }
    @Override public void setNetworkAvailable(boolean networkUp) {}
    @Override public WebBackForwardList saveState(Bundle outState) { return null; }
    @Override public boolean savePicture(Bundle b, File dest) { return false; }
    @Override public boolean restorePicture(Bundle b, File src) { return false; }
    @Override public WebBackForwardList restoreState(Bundle inState) { return null; }
    @Override
    public void loadUrl(String url, Map<String, String> additionalHttpHeaders) {
        try {
            JSONObject call = call("loadUrl").put("url", toHost(url));
            if (additionalHttpHeaders != null && !additionalHttpHeaders.isEmpty()) {
                call.put("headers", new JSONObject(additionalHttpHeaders));
            }
            load(call, null);
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    @Override public void loadUrl(String url) { loadUrl(url, null); }

    @Override
    public void postUrl(String url, byte[] postData) {
        try {
            load(call("postUrl").put("url", toHost(url))
                    .put("body", Base64.encodeToString(postData != null ? postData : new byte[0], Base64.NO_WRAP)),
                    "body");
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    @Override
    public void loadData(String data, String mimeType, String encoding) {
        try {
            load(call("loadData").put("data", data).put("mimeType", mimeType).put("encoding", encoding), "data");
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    @Override
    public void loadDataWithBaseURL(String baseUrl, String data, String mimeType, String encoding,
            String historyUrl) {
        try {
            load(call("loadDataWithBaseURL").put("baseUrl", toHost(baseUrl)).put("data", data)
                    .put("mimeType", mimeType).put("encoding", encoding).put("historyUrl", toHost(historyUrl)),
                    "data");
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    /** A script's value comes from the host by its request id; without a host, "null", as b1 answers. */
    @Override
    public void evaluateJavaScript(String script, ValueCallback<String> resultCallback) {
        requested();
        if (hostOpen()) {
            long id = mChannel.nextRequestId();
            if (resultCallback != null) {
                mScripts.put(id, resultCallback);
            }
            try {
                if (mChannel.call(id, call("evaluateJavascript").put("script", script), null) >= 0) {
                    return;
                }
            } catch (JSONException e) {
                throw new IllegalStateException(e);
            }
            mScripts.remove(id);
        }
        noContent();
        if (resultCallback != null) {
            new Handler(Looper.getMainLooper()).post(() -> resultCallback.onReceiveValue("null"));
        }
    }

    @Override public void saveWebArchive(String filename) {}

    @Override
    public void saveWebArchive(String basename, boolean autoname, ValueCallback<String> callback) {
        if (callback != null) {
            new Handler(Looper.getMainLooper()).post(() -> callback.onReceiveValue(null));
        }
    }

    @Override public void stopLoading() { command("stopLoading", null, null); }
    @Override public void reload() { command("reload", null, null); }
    @Override public boolean canGoBack() { return canGoBackOrForward(-1); }
    @Override public void goBack() { goBackOrForward(-1); }
    @Override public boolean canGoForward() { return canGoBackOrForward(1); }
    @Override public void goForward() { goBackOrForward(1); }

    @Override
    public boolean canGoBackOrForward(int steps) {
        int index = mHistoryIndex + steps;
        return steps == 0 || (mHistoryIndex >= 0 && index >= 0 && index < mHistory.size());
    }

    @Override
    public void goBackOrForward(int steps) {
        if (steps != 0 && canGoBackOrForward(steps)) {
            command("goBackOrForward", "steps", steps);
        }
    }
    @Override public boolean isPrivateBrowsingEnabled() { return false; }
    @Override public boolean pageUp(boolean top) { return false; }
    @Override public boolean pageDown(boolean bottom) { return false; }

    @Override
    public void insertVisualStateCallback(long requestId, WebView.VisualStateCallback callback) {
        new Handler(Looper.getMainLooper()).post(() -> callback.onComplete(requestId));
    }

    @Override public void clearView() {}
    @Override public Picture capturePicture() { return new Picture(); }
    @Override public PrintDocumentAdapter createPrintDocumentAdapter(String documentName) { return null; }
    @Override public float getScale() { return mWebView.getResources().getDisplayMetrics().density; }
    @Override public void setInitialScale(int scaleInPercent) {}
    @Override public void invokeZoomPicker() {}
    @Override public WebView.HitTestResult getHitTestResult() { return new WebView.HitTestResult(); }
    @Override public void requestFocusNodeHref(Message hrefMsg) {}
    @Override public void requestImageRef(Message msg) {}
    @Override public String getUrl() { return mUrl; }
    @Override public String getOriginalUrl() { return mOriginalUrl; }
    /** The page's, from the host; before any page, as Chromium's: an empty title, where the URL is null. */
    @Override public String getTitle() { return mTitle; }
    @Override public Bitmap getFavicon() { return null; }
    @Override public String getTouchIconUrl() { return null; }
    @Override public int getProgress() { return mProgress; }
    @Override public int getContentHeight() { return 0; }
    @Override public int getContentWidth() { return 0; }
    @Override public void pauseTimers() {}
    @Override public void resumeTimers() {}
    @Override public void onPause() { mPaused = true; }
    @Override public void onResume() { mPaused = false; }
    @Override public boolean isPaused() { return mPaused; }
    @Override public void freeMemory() {}
    @Override
    public void clearCache(boolean includeDiskFiles) {
        command("clearCache", "includeDiskFiles", includeDiskFiles);
    }

    @Override public void clearFormData() {}
    @Override public void clearHistory() {}
    @Override public void clearSslPreferences() {}
    @Override public WebBackForwardList copyBackForwardList() { return new History(mHistory, mHistoryIndex); }
    @Override public void setFindListener(WebView.FindListener listener) {}
    @Override public void findNext(boolean forward) {}
    @Override public int findAll(String find) { return 0; }
    @Override public void findAllAsync(String find) {}
    @Override public boolean showFindDialog(String text, boolean showIme) { return false; }
    @Override public void clearMatches() {}

    @Override
    public void documentHasImages(Message response) {
        response.arg1 = 0;
        response.sendToTarget();
    }

    @Override public void setWebViewClient(WebViewClient client) { mClient = client; }
    @Override public WebViewClient getWebViewClient() { return mClient; }
    @Override public WebViewRenderProcess getWebViewRenderProcess() { return null; }

    @Override
    public void setWebViewRenderProcessClient(Executor executor, WebViewRenderProcessClient client) {
        mRenderProcessClient = client;
    }

    @Override public WebViewRenderProcessClient getWebViewRenderProcessClient() { return mRenderProcessClient; }
    @Override public void setDownloadListener(DownloadListener listener) {}
    @Override public void setWebChromeClient(WebChromeClient client) { mChromeClient = client; }
    @Override public WebChromeClient getWebChromeClient() { return mChromeClient; }
    @Override public void setPictureListener(WebView.PictureListener listener) {}
    @Override
    public void addJavascriptInterface(Object obj, String interfaceName) {
        if (obj == null || interfaceName == null) {
            return;
        }
        synchronized (mInterfaces) {
            mInterfaces.put(interfaceName, obj);
        }
        sendInterfaces();
    }

    @Override
    public void removeJavascriptInterface(String interfaceName) {
        synchronized (mInterfaces) {
            mInterfaces.remove(interfaceName);
        }
        sendInterfaces();
    }

    /**
     * Whether a page sees a method of an app object: from target SDK 17 (JELLY_BEAN_MR1) on only one annotated
     * @JavascriptInterface, below every public one, as Chromium's AwContents.addJavascriptInterface passes the
     * annotation as required only from 17 on.
     */
    private boolean exposes(Method method) {
        return mWebView.getContext().getApplicationInfo().targetSdkVersion < Build.VERSION_CODES.JELLY_BEAN_MR1
                || method.isAnnotationPresent(JavascriptInterface.class);
    }

    /** The app's objects to the host, by name, with the methods a page sees (exposes): for the next pages. */
    private void sendInterfaces() {
        if (!hostOpen()) {
            return;
        }
        JSONObject objects = new JSONObject();
        try {
            synchronized (mInterfaces) {
                for (Map.Entry<String, Object> entry : mInterfaces.entrySet()) {
                    JSONArray methods = new JSONArray();
                    List<String> names = new ArrayList<>();
                    for (Method method : entry.getValue().getClass().getMethods()) {
                        if (exposes(method) && !names.contains(method.getName())) {
                            names.add(method.getName());
                            methods.put(method.getName());
                        }
                    }
                    objects.put(entry.getKey(), methods);
                }
            }
            send(call("interfaces").put("objects", objects), null, 0);
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    /**
     * A page's call of an app object, on the thread of the app's objects, as Chromium's Java bridge thread: the method
     * of its name and number of arguments that a page sees (exposes), the arguments converted to its parameters' types;
     * the answer {"value"} (none for void), or {"error"} as the page's exception. Object.getClass is blocked whatever
     * the target SDK, as Chromium's GinJavaMethodInvocationHelper blocks it.
     */
    private JSONObject invoke(JSONObject call) throws JSONException {
        Object target;
        synchronized (mInterfaces) {
            target = mInterfaces.get(call.optString("object"));
        }
        JSONArray args = call.optJSONArray("args");
        int count = args == null ? 0 : args.length();
        Method method = null;
        for (Method candidate : target == null ? new Method[0] : target.getClass().getMethods()) {
            if (candidate.getName().equals(call.optString("method")) && candidate.getParameterCount() == count
                    && exposes(candidate)) {
                method = candidate;
                break;
            }
        }
        JSONObject answer = new JSONObject();
        if (method == null) {
            return answer.put("error", "Method not found");
        }
        if (method.getName().equals("getClass") && count == 0) {
            return answer.put("error", "Access to java.lang.Object.getClass is blocked");
        }
        Class<?>[] types = method.getParameterTypes();
        Object[] values = new Object[count];
        for (int i = 0; i < count; i++) {
            values[i] = toJava(args.opt(i), types[i]);
        }
        Object result;
        try {
            result = method.invoke(target, values);
        } catch (ReflectiveOperationException | IllegalArgumentException e) {
            return answer.put("error", "Java exception was raised during method invocation");
        }
        if (method.getReturnType() != void.class) {
            answer.put("value", result instanceof Number || result instanceof Boolean || result instanceof String
                    ? result : result instanceof Character ? result.toString() : JSONObject.NULL);
        }
        return answer;
    }

    /** A page's argument as the parameter's type: a number for a primitive number, a string for a String. */
    private static Object toJava(Object value, Class<?> type) {
        Number number = value instanceof Number ? (Number) value : 0;
        if (type == int.class) return number.intValue();
        if (type == long.class) return number.longValue();
        if (type == double.class) return number.doubleValue();
        if (type == float.class) return number.floatValue();
        if (type == short.class) return number.shortValue();
        if (type == byte.class) return number.byteValue();
        if (type == char.class) return (char) number.intValue();
        if (type == boolean.class) return value instanceof Boolean && (Boolean) value;
        if (type == String.class) {
            return value == null || value == JSONObject.NULL ? null : value.toString();
        }
        return null;
    }
    @Override public WebMessagePort[] createWebMessageChannel() { return new WebMessagePort[0]; }
    @Override public void postMessageToMainFrame(WebMessage message, Uri targetOrigin) {}
    @Override public WebSettings getSettings() { return mSettings; }
    @Override public void setMapTrackballToArrowKeys(boolean setMap) {}
    @Override public void flingScroll(int vx, int vy) {}
    @Override public View getZoomControls() { return null; }
    @Override public boolean canZoomIn() { return false; }
    @Override public boolean canZoomOut() { return false; }
    @Override public boolean zoomBy(float zoomFactor) { return false; }
    @Override public boolean zoomIn() { return false; }
    @Override public boolean zoomOut() { return false; }
    @Override public void dumpViewHierarchyWithProperties(BufferedWriter out, int level) {}
    @Override public View findHierarchyView(String className, int hashCode) { return null; }

    @Override
    public void setRendererPriorityPolicy(int rendererRequestedPriority, boolean waivedWhenNotVisible) {
        mRendererPriority = rendererRequestedPriority;
        mRendererWaived = waivedWhenNotVisible;
    }

    @Override public int getRendererRequestedPriority() { return mRendererPriority; }
    @Override public boolean getRendererPriorityWaivedWhenNotVisible() { return mRendererWaived; }
    @Override public ViewDelegate getViewDelegate() { return this; }
    @Override public ScrollDelegate getScrollDelegate() { return this; }
    @Override public void notifyFindDialogDismissed() {}

    // ViewDelegate

    @Override public boolean shouldDelayChildPressedState() { return true; }
    @Override public void onProvideVirtualStructure(ViewStructure structure) {}
    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider() { return null; }
    @Override public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {}
    @Override public void onInitializeAccessibilityEvent(AccessibilityEvent event) {}

    @Override
    public boolean performAccessibilityAction(int action, Bundle arguments) {
        return mPrivate.super_performAccessibilityAction(action, arguments);
    }

    @Override public void setOverScrollMode(int mode) {}
    @Override public void setScrollBarStyle(int style) {}

    @Override
    public void onDrawVerticalScrollBar(Canvas canvas, Drawable scrollBar, int l, int t, int r, int b) {
        mPrivate.super_onDrawVerticalScrollBar(canvas, scrollBar, l, t, r, b);
    }

    @Override public void onOverScrolled(int scrollX, int scrollY, boolean clampedX, boolean clampedY) {}
    @Override public void onWindowVisibilityChanged(int visibility) {}

    /**
     * The page's area: cleared for the host's view when there is one; else empty, black before any content was asked
     * for, then the background color.
     */
    @Override
    public void onDraw(Canvas canvas) {
        if (mHost == HOST_OPEN) {
            canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);
            return;
        }
        int color = mContentRequested ? mBackgroundColor : Color.BLACK;
        if (Color.alpha(color) != 0) {
            mBackgroundPaint.setColor(color);
            canvas.drawRect(mWebView.getScrollX(), mWebView.getScrollY(),
                    mWebView.getScrollX() + mWebView.getWidth(), mWebView.getScrollY() + mWebView.getHeight(),
                    mBackgroundPaint);
        }
    }

    @Override
    public void setLayoutParams(ViewGroup.LayoutParams layoutParams) {
        mPrivate.super_setLayoutParams(layoutParams);
    }

    @Override public boolean performLongClick() { return mPrivate.super_performLongClick(); }
    @Override public void onConfigurationChanged(Configuration newConfig) {}
    @Override public InputConnection onCreateInputConnection(EditorInfo outAttrs) { return null; }
    @Override public boolean onDragEvent(DragEvent event) { return false; }
    @Override public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) { return false; }
    @Override public boolean onKeyDown(int keyCode, KeyEvent event) { return false; }
    @Override public boolean onKeyUp(int keyCode, KeyEvent event) { return false; }
    /** The host's view is opened here at the latest (a WebView never attached costs no channel), and placed. */
    @Override
    public void onAttachedToWindow() {
        hostOpen();
        mWebView.getViewTreeObserver().addOnPreDrawListener(mPlacer);
    }

    @Override
    public void onDetachedFromWindow() {
        mWebView.getViewTreeObserver().removeOnPreDrawListener(mPlacer);
    }
    @Override public void onVisibilityChanged(View changedView, int visibility) {}
    @Override public void onWindowFocusChanged(boolean hasWindowFocus) {}
    @Override public void onFocusChanged(boolean focused, int direction, Rect previouslyFocusedRect) {}

    @Override
    public boolean setFrame(int left, int top, int right, int bottom) {
        return mPrivate.super_setFrame(left, top, right, bottom);
    }

    @Override public void onSizeChanged(int w, int h, int ow, int oh) {}
    @Override public void onScrollChanged(int l, int t, int oldl, int oldt) {}
    @Override public boolean dispatchKeyEvent(KeyEvent event) { return mPrivate.super_dispatchKeyEvent(event); }
    @Override public boolean onTouchEvent(MotionEvent ev) { return false; }
    @Override public boolean onHoverEvent(MotionEvent event) { return mPrivate.super_onHoverEvent(event); }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        return mPrivate.super_onGenericMotionEvent(event);
    }

    @Override public boolean onTrackballEvent(MotionEvent ev) { return false; }

    @Override
    public boolean requestFocus(int direction, Rect previouslyFocusedRect) {
        return mPrivate.super_requestFocus(direction, previouslyFocusedRect);
    }

    /**
     * As Chromium's AwLayoutSizer with no content: the width the parent offers, and a height that is the parent's
     * exact one, else the content's, which is 0 while there is no page.
     */
    @Override
    public void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = View.MeasureSpec.getMode(widthMeasureSpec) == View.MeasureSpec.UNSPECIFIED ? 0
                : View.MeasureSpec.getSize(widthMeasureSpec);
        int height = View.MeasureSpec.getMode(heightMeasureSpec) == View.MeasureSpec.EXACTLY
                ? View.MeasureSpec.getSize(heightMeasureSpec) : 0;
        mPrivate.setMeasuredDimension(width, height);
    }

    @Override public boolean requestChildRectangleOnScreen(View child, Rect rect, boolean immediate) { return false; }

    @Override
    public void setBackgroundColor(int color) {
        mBackgroundColor = color;
        mWebView.invalidate();
    }

    @Override public void setLayerType(int layerType, Paint paint) {}
    @Override public void preDispatchDraw(Canvas canvas) {}
    @Override public void onStartTemporaryDetach() {}
    @Override public void onFinishTemporaryDetach() {}
    @Override public void onActivityResult(int requestCode, int resultCode, Intent data) {}
    @Override public Handler getHandler(Handler originalHandler) { return originalHandler; }
    @Override public View findFocus(View originalFocusedView) { return originalFocusedView; }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        return mPrivate.super_onApplyWindowInsets(insets);
    }

    // ScrollDelegate: no content, so the view's own size and no offset.

    @Override public int computeHorizontalScrollRange() { return mWebView.getWidth(); }
    @Override public int computeHorizontalScrollOffset() { return 0; }
    @Override public int computeVerticalScrollRange() { return mWebView.getHeight(); }
    @Override public int computeVerticalScrollOffset() { return 0; }
    @Override public int computeVerticalScrollExtent() { return mWebView.getHeight(); }
    @Override public void computeScroll() {}
}
