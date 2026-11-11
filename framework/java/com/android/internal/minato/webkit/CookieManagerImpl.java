package com.android.internal.minato.webkit;

import android.app.ActivityThread;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.webkit.CookieManager;
import android.webkit.ValueCallback;
import android.webkit.WebView;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The process's cookies (ADR-0056 decision 7, step c3): a mirror of the package's data store on the host, which a
 * channel of the manager's own (a "make" with "cookies", no view) opens at the first use and keeps for the process.
 * The host posts the store's whole list (COOKIES) at once, after each change and before each page's onPageFinished,
 * on the channel's pump; the first use waits for the first list, up to a second. setCookie and removeAllCookies change
 * the mirror at once and go to the host as CALLs; until the host answers one, it lies over each list that comes, so a
 * list the host made before it cannot take it back. getCookie answers as Chromium's does: a host-only cookie for its
 * host alone, a domain cookie for its subdomains too, the path by RFC 6265's rule, a secure one for https only,
 * HttpOnly ones too, no expired one; the longest path first, then the oldest. Without a host (no WebView in the
 * session) the mirror is all there is, for the process only. Third-party cookies are a setting of each WebView, kept by
 * its settings, as Chromium's AwCookieManager does.
 */
@SuppressWarnings("deprecation") // CookieManager's deprecated members, which Chromium's implements too.
final class CookieManagerImpl extends CookieManager {
    private static final long FIRST_LIST_MS = 1000;

    private static final class Cookie {
        String name;
        String value;
        String domain;     // lower case, no leading dot
        boolean hostOnly;
        String path;
        boolean secure;
        boolean httpOnly;
        long expires;      // ms since the epoch, or -1 for a session cookie
        long created;      // the order of creation: the host's ms since 2001 (NSHTTPCookie's Created), or later

        boolean sameAs(Cookie other) {
            return name.equals(other.name) && domain.equals(other.domain) && path.equals(other.path);
        }
    }

    /** An app's change the host has not answered yet: a cookie set (deleted, when expired), or all gone. */
    private static final class Change {
        final long requestId;
        final Cookie cookie;  // null: removeAllCookies

        Change(long requestId, Cookie cookie) {
            this.requestId = requestId;
            this.cookie = cookie;
        }
    }

    private final List<Cookie> mStore = new ArrayList<>();
    private final List<Change> mChanges = new ArrayList<>();
    private final Map<Long, ValueCallback<Boolean>> mCallbacks = new HashMap<>();
    private final CountDownLatch mFirstList = new CountDownLatch(1);
    private long mCreated;
    private boolean mOpened;
    private WebViewChannel mChannel;
    private boolean mAcceptCookie = true;
    private boolean mAcceptFileSchemeCookies;

    private static void answer(ValueCallback<Boolean> callback, boolean value) {
        if (callback != null) {
            new Handler(Looper.getMainLooper()).post(() -> callback.onReceiveValue(value));
        }
    }

    /** The channel to the host, opened at the first use; null without a host. Waits for its first list. */
    private WebViewChannel channel() {
        WebViewChannel channel;
        synchronized (this) {
            if (!mOpened) {
                mOpened = true;
                open();
            }
            channel = mChannel;
        }
        if (channel != null) {
            try {
                mFirstList.await(FIRST_LIST_MS, TimeUnit.MILLISECONDS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            // Once: a host that posts no list leaves the mirror as it is, without a second's wait on each call.
            mFirstList.countDown();
        }
        return channel;
    }

    private void open() {
        WebViewChannel.sCookies = this::onCookies;
        WebViewChannel channel = new WebViewChannel();
        int made = channel.make(new WebViewChannel.Records() {
            @Override
            public void onRecord(int type, long requestId, JSONObject payload) {
                if (type == WebViewChannel.REC_JS_RESULT) {
                    answered(requestId, "true".equals(payload.optString("json")));
                }
            }

            @Override
            public void onResource(long requestId, JSONObject request) {}

            @Override
            public void onChannelEnd() {
                synchronized (CookieManagerImpl.this) {
                    mChannel = null;
                    mOpened = false;
                }
            }
        });
        if (made < 0) {
            return;
        }
        String store = UUID.nameUUIDFromBytes(("minato-webview:" + ActivityThread.currentPackageName())
                .getBytes(StandardCharsets.UTF_8)).toString();
        try {
            if (channel.call(0, new JSONObject().put("call", "make").put("dataStore", store).put("cookies", true),
                    null) < 0) {
                channel.remove();
                return;
            }
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
        mChannel = channel;
    }

    /** A COOKIES record, on a channel's pump: the store's list, with the changes the host has not answered over it. */
    private void onCookies(JSONObject record) {
        JSONArray list = record.optJSONArray("cookies");
        synchronized (this) {
            mStore.clear();
            for (int i = 0; list != null && i < list.length(); i++) {
                JSONObject item = list.optJSONObject(i);
                if (item == null) {
                    continue;
                }
                Cookie cookie = new Cookie();
                cookie.name = item.optString("name");
                cookie.value = item.optString("value");
                cookie.domain = trimDot(item.optString("domain").toLowerCase(Locale.ROOT));
                cookie.hostOnly = item.optBoolean("hostOnly");
                cookie.path = item.optString("path", "/");
                cookie.secure = item.optBoolean("secure");
                cookie.httpOnly = item.optBoolean("httpOnly");
                cookie.expires = item.isNull("expires") ? -1 : item.optLong("expires", -1);
                cookie.created = item.optLong("created", i);
                mStore.add(cookie);
            }
        }
        mFirstList.countDown();
    }

    /** The host's answer to a change: it is in the store's lists from now on. */
    private void answered(long requestId, boolean result) {
        ValueCallback<Boolean> callback;
        synchronized (this) {
            mChanges.removeIf(change -> change.requestId == requestId);
            callback = mCallbacks.remove(requestId);
        }
        if (callback != null) {
            callback.onReceiveValue(result);
        }
    }

    /** A change the host never got: it goes, and its callback hears false on the main Looper, as answers do. */
    private void failed(long requestId) {
        ValueCallback<Boolean> callback;
        synchronized (this) {
            mChanges.removeIf(change -> change.requestId == requestId);
            callback = mCallbacks.remove(requestId);
        }
        answer(callback, false);
    }

    /** The cookies now: the store's last list with the unanswered changes over it, in order. */
    private List<Cookie> cookies() {
        List<Cookie> all = new ArrayList<>(mStore);
        for (Change change : mChanges) {
            if (change.cookie == null) {
                all.clear();
            } else {
                all.removeIf(cookie -> cookie.sameAs(change.cookie));
                if (change.cookie.expires < 0 || change.cookie.expires > System.currentTimeMillis()) {
                    all.add(change.cookie);
                }
            }
        }
        return all;
    }

    private static String trimDot(String domain) {
        return domain.startsWith(".") ? domain.substring(1) : domain;
    }

    /** RFC 6265 5.1.4's default path of `url`. */
    private static String defaultPath(Uri uri) {
        String path = uri.getPath();
        if (path == null || !path.startsWith("/")) {
            return "/";
        }
        int last = path.lastIndexOf('/');
        return last <= 0 ? "/" : path.substring(0, last);
    }

    /**
     * `value` set at `url` as a Set-Cookie header would: null when it cannot be (a domain the URL is not in, or a
     * Secure cookie from a URL that is not https, which Chromium refuses too). A public suffix as the domain is taken.
     */
    private Cookie parse(String url, String value) {
        Uri uri = url == null ? null : Uri.parse(url);
        String host = uri == null || uri.getHost() == null ? null : uri.getHost().toLowerCase(Locale.ROOT);
        if (host == null || value == null) {
            return null;
        }
        String[] parts = value.split(";");
        Cookie cookie = new Cookie();
        int equals = parts[0].indexOf('=');
        cookie.name = (equals < 0 ? "" : parts[0].substring(0, equals)).trim();
        cookie.value = (equals < 0 ? parts[0] : parts[0].substring(equals + 1)).trim();
        cookie.domain = host;
        cookie.hostOnly = true;
        cookie.path = defaultPath(uri);
        cookie.expires = -1;
        long maxAge = Long.MIN_VALUE;
        for (int i = 1; i < parts.length; i++) {
            String attribute = parts[i].trim();
            int at = attribute.indexOf('=');
            String key = (at < 0 ? attribute : attribute.substring(0, at)).trim().toLowerCase(Locale.ROOT);
            String argument = at < 0 ? "" : attribute.substring(at + 1).trim();
            switch (key) {
                case "domain":
                    String domain = trimDot(argument.toLowerCase(Locale.ROOT));
                    if (!domain.isEmpty()) {
                        if (!host.equals(domain) && !host.endsWith("." + domain)) {
                            return null;
                        }
                        cookie.domain = domain;
                        cookie.hostOnly = false;
                    }
                    break;
                case "path":
                    if (argument.startsWith("/")) {
                        cookie.path = argument;
                    }
                    break;
                case "secure":
                    cookie.secure = true;
                    break;
                case "httponly":
                    cookie.httpOnly = true;
                    break;
                case "max-age":
                    try {
                        maxAge = Long.parseLong(argument);
                    } catch (NumberFormatException e) {
                        // ignored, as RFC 6265 5.2.2 says
                    }
                    break;
                case "expires":
                    try {
                        SimpleDateFormat format = new SimpleDateFormat("EEE, dd MMM yyyy HH:mm:ss zzz", Locale.US);
                        format.setTimeZone(TimeZone.getTimeZone("GMT"));
                        cookie.expires = format.parse(argument).getTime();
                    } catch (ParseException e) {
                        // ignored, as RFC 6265 5.2.1 says of a date it cannot read
                    }
                    break;
                default:
                    break;
            }
        }
        if (cookie.secure && !trustworthy(uri)) {
            return null;
        }
        if (maxAge != Long.MIN_VALUE) {
            cookie.expires = maxAge <= 0 ? 0 : System.currentTimeMillis() + maxAge * 1000;
        }
        cookie.created = Long.MAX_VALUE / 2 + ++mCreated;
        return cookie;
    }

    @Override public synchronized void setAcceptCookie(boolean accept) { mAcceptCookie = accept; }
    @Override public synchronized boolean acceptCookie() { return mAcceptCookie; }

    @Override
    public void setAcceptThirdPartyCookies(WebView webview, boolean accept) {
        webview.getSettings().setAcceptThirdPartyCookies(accept);
    }

    @Override
    public boolean acceptThirdPartyCookies(WebView webview) {
        return webview.getSettings().getAcceptThirdPartyCookies();
    }

    @Override
    public void setCookie(String url, String value) {
        setCookie(url, value, null);
    }

    @Override
    public void setCookie(String url, String value, ValueCallback<Boolean> callback) {
        WebViewChannel channel = channel();
        Cookie cookie;
        synchronized (this) {
            cookie = mAcceptCookie ? parse(url, value) : null;
            if (cookie == null) {
                answer(callback, false);
                return;
            }
            if (channel == null) {
                mStore.removeIf(old -> old.sameAs(cookie));
                if (cookie.expires < 0 || cookie.expires > System.currentTimeMillis()) {
                    mStore.add(cookie);
                }
                answer(callback, true);
                return;
            }
        }
        long requestId = channel.nextRequestId();
        synchronized (this) {
            mChanges.add(new Change(requestId, cookie));
            if (callback != null) {
                mCallbacks.put(requestId, callback);
            }
        }
        try {
            if (channel.call(requestId, new JSONObject().put("call", "setCookie").put("url", url).put("value", value),
                    null) < 0) {
                failed(requestId);
            }
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    @Override
    public String getCookie(String url) {
        channel();
        Uri uri = url == null ? null : Uri.parse(url);
        String host = uri == null || uri.getHost() == null ? null : uri.getHost().toLowerCase(Locale.ROOT);
        if (host == null) {
            return null;
        }
        String path = uri.getPath() == null || uri.getPath().isEmpty() ? "/" : uri.getPath();
        boolean secure = trustworthy(uri);
        long now = System.currentTimeMillis();
        List<Cookie> matching = new ArrayList<>();
        synchronized (this) {
            for (Cookie cookie : cookies()) {
                boolean hostMatches = cookie.hostOnly ? host.equals(cookie.domain)
                        : host.equals(cookie.domain) || host.endsWith("." + cookie.domain);
                boolean pathMatches = path.equals(cookie.path) || (path.startsWith(cookie.path)
                        && (cookie.path.endsWith("/") || path.charAt(cookie.path.length()) == '/'));
                if (hostMatches && pathMatches && (secure || !cookie.secure)
                        && (cookie.expires < 0 || cookie.expires > now)) {
                    matching.add(cookie);
                }
            }
        }
        if (matching.isEmpty()) {
            return null;
        }
        matching.sort((a, b) -> a.path.length() != b.path.length() ? b.path.length() - a.path.length()
                : Long.compare(a.created, b.created));
        List<String> pairs = new ArrayList<>();
        for (Cookie cookie : matching) {
            pairs.add(cookie.name.isEmpty() ? cookie.value : cookie.name + "=" + cookie.value);
        }
        return String.join("; ", pairs);
    }

    @Override
    public String getCookie(String url, boolean privateBrowsing) {
        return getCookie(url);
    }

    @Override
    public void removeSessionCookie() {
        removeSessionCookies(null);
    }

    /** Session cookies go from the mirror only; the host's store keeps them until WebKit's session ends. */
    @Override
    public void removeSessionCookies(ValueCallback<Boolean> callback) {
        channel();
        boolean removed;
        synchronized (this) {
            removed = mStore.removeIf(cookie -> cookie.expires < 0);
        }
        answer(callback, removed);
    }

    @Override
    public void removeAllCookie() {
        removeAllCookies(null);
    }

    @Override
    public void removeAllCookies(ValueCallback<Boolean> callback) {
        WebViewChannel channel = channel();
        if (channel == null) {
            boolean removed;
            synchronized (this) {
                removed = !mStore.isEmpty();
                mStore.clear();
            }
            answer(callback, removed);
            return;
        }
        long requestId = channel.nextRequestId();
        synchronized (this) {
            mChanges.add(new Change(requestId, null));
            if (callback != null) {
                mCallbacks.put(requestId, callback);
            }
        }
        try {
            if (channel.call(requestId, new JSONObject().put("call", "removeAllCookies"), null) < 0) {
                failed(requestId);
            }
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    @Override
    public boolean hasCookies() {
        channel();
        synchronized (this) {
            return !cookies().isEmpty();
        }
    }

    @Override
    public boolean hasCookies(boolean privateBrowsing) {
        return hasCookies();
    }

    @Override public void removeExpiredCookie() {}
    @Override public void flush() {}
    @Override protected synchronized boolean allowFileSchemeCookiesImpl() { return mAcceptFileSchemeCookies; }
    @Override protected synchronized void setAcceptFileSchemeCookiesImpl(boolean accept) {
        mAcceptFileSchemeCookies = accept;
    }

    /** Whether cookies count the URL as secure: https or wss, or the loopback (Chromium's trustworthy origins). */
    private static boolean trustworthy(Uri uri) {
        String scheme = uri.getScheme() == null ? "" : uri.getScheme().toLowerCase(Locale.ROOT);
        String host = uri.getHost() == null ? "" : uri.getHost().toLowerCase(Locale.ROOT);
        return scheme.equals("https") || scheme.equals("wss") || host.equals("localhost")
                || host.endsWith(".localhost") || host.equals("127.0.0.1") || host.equals("[::1]")
                || host.equals("::1");
    }
}
