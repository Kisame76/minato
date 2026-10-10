/*
 * Ours in place of AOSP's WebViewFactory (the Unity test APK probe's batch C): the WebView provider's loader, as Android 15's
 * answers it on a device without the webview feature (PackageManager.FEATURE_WEBVIEW), which Minato does not report
 * (SystemFeatures). AOSP's getProvider throws UnsupportedOperationException without a message there, before it looks
 * for a provider package, and getUpdateService answers null; so a new WebView, CookieManager.getInstance and
 * WebSettings.getDefaultUserAgent throw, and WebView.getCurrentWebViewPackage answers null [verified:
 * WebViewFactory.java at android-15.0.0_r1]. With the feature, AOSP would load the package WebViewUpdateService
 * names; Minato has none, so getProvider then fails as AOSP does for a device without the package. The data
 * directory suffix and disableWebView keep AOSP's checks. Kept out: the provider's loading, its native library and
 * RELRO, the zygote and the update service, which only a provider package needs (the real WebView on WKWebView is a
 * later ADR).
 */
package android.webkit;

import android.app.AppGlobals;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.util.AndroidRuntimeException;
import android.util.Log;

import java.io.File;

/** @hide */
public final class WebViewFactory {
    private static final String LOGTAG = "WebViewFactory";

    private static final Object sProviderLock = new Object();
    private static Boolean sWebViewSupported;
    private static boolean sWebViewDisabled;
    private static String sDataDirectorySuffix;

    private WebViewFactory() {}

    @SuppressWarnings("serial")
    static class MissingWebViewPackageException extends Exception {
        MissingWebViewPackageException(String message) {
            super(message);
        }
    }

    private static boolean isWebViewSupported() {
        if (sWebViewSupported == null) {
            sWebViewSupported = AppGlobals.getInitialApplication().getPackageManager()
                    .hasSystemFeature(PackageManager.FEATURE_WEBVIEW);
        }
        return sWebViewSupported;
    }

    /** No provider is ever loaded. */
    public static PackageInfo getLoadedPackageInfo() {
        return null;
    }

    static void setDataDirectorySuffix(String suffix) {
        synchronized (sProviderLock) {
            if (suffix.indexOf(File.separatorChar) >= 0) {
                throw new IllegalArgumentException("Suffix " + suffix + " contains a path separator");
            }
            sDataDirectorySuffix = suffix;
        }
    }

    static String getDataDirectorySuffix() {
        synchronized (sProviderLock) {
            return sDataDirectorySuffix;
        }
    }

    static void disableWebView() {
        synchronized (sProviderLock) {
            sWebViewDisabled = true;
        }
    }

    static WebViewFactoryProvider getProvider() {
        synchronized (sProviderLock) {
            if (!isWebViewSupported()) {
                // Device doesn't support WebView; don't try to load it, just throw.
                throw new UnsupportedOperationException();
            }
            if (sWebViewDisabled) {
                throw new IllegalStateException("WebView.disableWebView() was called: WebView is disabled");
            }
            MissingWebViewPackageException e = new MissingWebViewPackageException(
                    "Failed to load WebView provider: No WebView installed");
            Log.e(LOGTAG, "Chromium WebView package does not exist", e);
            throw new AndroidRuntimeException(e);
        }
    }

    /** The update service, which only a device with the webview feature has. */
    public static IWebViewUpdateService getUpdateService() {
        return null;
    }
}
