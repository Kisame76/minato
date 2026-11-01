package com.android.internal.minato.webkit;

import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.webkit.CookieManager;
import android.webkit.ValueCallback;
import android.webkit.WebView;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The process's cookie store (ADR-0056 decision 7) until step c) mirrors the page's: the cookies the app sets, by host,
 * read back as Chromium's getCookie gives them ("name=value; name2=value2"). Cookies are kept for the process only,
 * and a session cookie is one without Expires or Max-Age. Domain, path and expiry are not matched yet; third-party
 * cookies are a setting of each WebView, kept by its settings, as Chromium's AwCookieManager does.
 */
@SuppressWarnings("deprecation") // CookieManager's deprecated members, which Chromium's implements too.
final class CookieManagerImpl extends CookieManager {
    private static final class Cookie {
        final String value;
        final boolean session;

        Cookie(String value, boolean session) {
            this.value = value;
            this.session = session;
        }
    }

    private final Map<String, Map<String, Cookie>> mHosts = new LinkedHashMap<>();
    private boolean mAcceptCookie = true;
    private boolean mAcceptFileSchemeCookies;

    private static String host(String url) {
        String host = url == null ? null : Uri.parse(url).getHost();
        return host == null ? null : host.toLowerCase(Locale.ROOT);
    }

    private static void answer(ValueCallback<Boolean> callback, boolean value) {
        if (callback != null) {
            new Handler(Looper.getMainLooper()).post(() -> callback.onReceiveValue(value));
        }
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
        boolean set = false;
        String host = host(url);
        if (host != null && value != null) {
            String[] parts = value.split(";");
            int equals = parts[0].indexOf('=');
            String name = (equals < 0 ? "" : parts[0].substring(0, equals)).trim();
            String pair = parts[0].trim();
            boolean session = true;
            for (int i = 1; i < parts.length; i++) {
                String attribute = parts[i].trim().toLowerCase(Locale.ROOT);
                if (attribute.startsWith("expires=") || attribute.startsWith("max-age=")) {
                    session = false;
                }
            }
            synchronized (this) {
                if (mAcceptCookie) {
                    mHosts.computeIfAbsent(host, h -> new LinkedHashMap<>()).put(name, new Cookie(pair, session));
                    set = true;
                }
            }
        }
        answer(callback, set);
    }

    @Override
    public synchronized String getCookie(String url) {
        Map<String, Cookie> cookies = mHosts.get(host(url));
        if (cookies == null || cookies.isEmpty()) {
            return null;
        }
        List<String> pairs = new ArrayList<>();
        for (Cookie cookie : cookies.values()) {
            pairs.add(cookie.value);
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

    @Override
    public void removeSessionCookies(ValueCallback<Boolean> callback) {
        boolean removed = false;
        synchronized (this) {
            for (Map<String, Cookie> cookies : mHosts.values()) {
                removed |= cookies.values().removeIf(cookie -> cookie.session);
            }
        }
        answer(callback, removed);
    }

    @Override
    public void removeAllCookie() {
        removeAllCookies(null);
    }

    @Override
    public void removeAllCookies(ValueCallback<Boolean> callback) {
        boolean removed;
        synchronized (this) {
            removed = hasCookies();
            mHosts.clear();
        }
        answer(callback, removed);
    }

    @Override
    public synchronized boolean hasCookies() {
        for (Map<String, Cookie> cookies : mHosts.values()) {
            if (!cookies.isEmpty()) {
                return true;
            }
        }
        return false;
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
}
