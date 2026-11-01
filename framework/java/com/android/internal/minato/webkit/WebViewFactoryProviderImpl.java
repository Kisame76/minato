package com.android.internal.minato.webkit;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.webkit.CookieManager;
import android.webkit.GeolocationPermissions;
import android.webkit.ServiceWorkerClient;
import android.webkit.ServiceWorkerController;
import android.webkit.ServiceWorkerWebSettings;
import android.webkit.TokenBindingService;
import android.webkit.TracingConfig;
import android.webkit.TracingController;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebIconDatabase;
import android.webkit.WebSettings;
import android.webkit.WebStorage;
import android.webkit.WebView;
import android.webkit.WebViewDatabase;
import android.webkit.WebViewFactory;
import android.webkit.WebViewFactoryProvider;
import android.webkit.WebViewProvider;

import java.io.OutputStream;
import java.util.List;
import java.util.concurrent.Executor;

/**
 * Minato's WebView provider factory (ADR-0056 decision 1), which our WebViewFactory gives where Android loads
 * Chromium's: each WebView gets a WebViewProviderImpl, and the process-wide objects answer as Chromium's do before any
 * page has loaded. The page itself is the host's WKWebView from step c) on.
 */
@SuppressWarnings("deprecation") // The interface's deprecated members, which Chromium's implements too.
public final class WebViewFactoryProviderImpl implements WebViewFactoryProvider {
    private final CookieManager mCookieManager = new CookieManagerImpl();
    private final GeolocationPermissions mGeolocationPermissions = new GeolocationPermissions();
    private final WebStorage mWebStorage = new WebStorage();
    private final ServiceWorkerController mServiceWorkerController = new ServiceWorkers();
    private final TracingController mTracingController = new Tracing();
    private WebViewDatabase mWebViewDatabase;

    private final Statics mStatics = new Statics() {
        @Override
        public String findAddress(String addr) {
            return WebViewFactory.findAddress(addr);
        }

        @Override
        public String getDefaultUserAgent(Context context) {
            return WebViewPackage.userAgent();
        }

        @Override public void freeMemoryForTests() {}
        @Override public void setWebContentsDebuggingEnabled(boolean enable) {}

        @Override
        public void clearClientCertPreferences(Runnable onCleared) {
            if (onCleared != null) {
                new Handler(Looper.getMainLooper()).post(onCleared);
            }
        }

        @Override public void enableSlowWholeDocumentDraw() {}

        @Override
        public Uri[] parseFileChooserResult(int resultCode, Intent intent) {
            return WebChromeClient.FileChooserParams.parseResult(resultCode, intent);
        }

        @Override
        public void initSafeBrowsing(Context context, ValueCallback<Boolean> callback) {
            // No Safe Browsing service on this device, as on a phone without Play services.
            if (callback != null) {
                new Handler(Looper.getMainLooper()).post(() -> callback.onReceiveValue(false));
            }
        }

        @Override
        public void setSafeBrowsingWhitelist(List<String> hosts, ValueCallback<Boolean> callback) {
            if (callback != null) {
                new Handler(Looper.getMainLooper()).post(() -> callback.onReceiveValue(true));
            }
        }

        @Override
        public Uri getSafeBrowsingPrivacyPolicyUrl() {
            return Uri.parse("https://www.google.com/chrome/privacy/");
        }
    };

    @Override public Statics getStatics() { return mStatics; }

    @Override
    public WebViewProvider createWebView(WebView webView, WebView.PrivateAccess privateAccess) {
        return new WebViewProviderImpl(webView, privateAccess);
    }

    @Override public GeolocationPermissions getGeolocationPermissions() { return mGeolocationPermissions; }
    @Override public CookieManager getCookieManager() { return mCookieManager; }
    @Override public TokenBindingService getTokenBindingService() { return null; }
    @Override public TracingController getTracingController() { return mTracingController; }
    @Override public ServiceWorkerController getServiceWorkerController() { return mServiceWorkerController; }
    @Override public WebIconDatabase getWebIconDatabase() { return null; }
    @Override public WebStorage getWebStorage() { return mWebStorage; }

    @Override
    public synchronized WebViewDatabase getWebViewDatabase(Context context) {
        if (mWebViewDatabase == null) {
            mWebViewDatabase = new Database();
        }
        return mWebViewDatabase;
    }

    @Override public ClassLoader getWebViewClassLoader() { return WebViewFactoryProviderImpl.class.getClassLoader(); }

    /** No saved passwords or form data: nothing is kept yet. */
    private static final class Database extends WebViewDatabase {
        @Override public boolean hasUsernamePassword() { return false; }
        @Override public void clearUsernamePassword() {}
        @Override public boolean hasHttpAuthUsernamePassword() { return false; }
        @Override public void clearHttpAuthUsernamePassword() {}
        @Override public void setHttpAuthUsernamePassword(String host, String realm, String username,
                String password) {}
        @Override public String[] getHttpAuthUsernamePassword(String host, String realm) { return null; }
        @Override public boolean hasFormData() { return false; }
        @Override public void clearFormData() {}
    }

    /** Service workers' settings, kept with WebSettings' defaults; no worker runs before step c). */
    private static final class ServiceWorkers extends ServiceWorkerController {
        private final ServiceWorkerWebSettings mSettings = new ServiceWorkerWebSettings() {
            private int mCacheMode = WebSettings.LOAD_DEFAULT;
            private boolean mAllowContentAccess = true;
            private boolean mAllowFileAccess = true;
            private boolean mBlockNetworkLoads;

            @Override public synchronized void setCacheMode(int mode) { mCacheMode = mode; }
            @Override public synchronized int getCacheMode() { return mCacheMode; }
            @Override public synchronized void setAllowContentAccess(boolean allow) { mAllowContentAccess = allow; }
            @Override public synchronized boolean getAllowContentAccess() { return mAllowContentAccess; }
            @Override public synchronized void setAllowFileAccess(boolean allow) { mAllowFileAccess = allow; }
            @Override public synchronized boolean getAllowFileAccess() { return mAllowFileAccess; }
            @Override public synchronized void setBlockNetworkLoads(boolean flag) { mBlockNetworkLoads = flag; }
            @Override public synchronized boolean getBlockNetworkLoads() { return mBlockNetworkLoads; }
        };

        @Override public ServiceWorkerWebSettings getServiceWorkerWebSettings() { return mSettings; }
        @Override public void setServiceWorkerClient(ServiceWorkerClient client) {}
    }

    /** Tracing as Chromium's controller answers it, without a trace to write. */
    private static final class Tracing extends TracingController {
        private boolean mTracing;

        @Override
        public synchronized void start(TracingConfig tracingConfig) {
            if (tracingConfig == null) {
                throw new IllegalArgumentException("tracingConfig cannot be null");
            }
            if (mTracing) {
                throw new IllegalStateException("cannot start tracing: tracing is already enabled");
            }
            mTracing = true;
        }

        @Override
        public synchronized boolean stop(OutputStream outputStream, Executor executor) {
            if (!mTracing) {
                return false;
            }
            mTracing = false;
            return true;
        }

        @Override public synchronized boolean isTracing() { return mTracing; }
    }
}
