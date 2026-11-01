/*
 * Ours in place of AOSP's WebViewFactory (the Unity test APK probe's batch C; ADR-0056 step b1): the WebView provider's
 * loader. Minato reports the webview feature (PackageManager.FEATURE_WEBVIEW, SystemFeatures), and its provider is
 * com.android.internal.minato.webkit's, in framework.jar, where AOSP loads the one of the package WebViewUpdateService
 * names (Chromium's). The loaded package is the one a phone has, com.google.android.webview (WebViewPackage), which
 * WebView.getCurrentWebViewPackage reads here first. Without the feature, AOSP's getProvider throws
 * UnsupportedOperationException without a message before it looks for a package [verified: WebViewFactory.java at
 * android-15.0.0_r1], which stays. The data directory suffix and disableWebView keep AOSP's checks. Kept out: the
 * provider package's loading, its native library and RELRO, the zygote and the update service.
 */
package android.webkit;

import android.app.AppGlobals;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;

import com.android.internal.minato.webkit.WebViewFactoryProviderImpl;
import com.android.internal.minato.webkit.WebViewPackage;

import java.io.File;

/** @hide */
public final class WebViewFactory {
    private static final Object sProviderLock = new Object();
    private static Boolean sWebViewSupported;
    private static boolean sWebViewDisabled;
    private static WebViewFactoryProvider sProviderInstance;
    private static String sDataDirectorySuffix;

    private WebViewFactory() {}

    private static boolean isWebViewSupported() {
        if (sWebViewSupported == null) {
            sWebViewSupported = AppGlobals.getInitialApplication().getPackageManager()
                    .hasSystemFeature(PackageManager.FEATURE_WEBVIEW);
        }
        return sWebViewSupported;
    }

    /** The provider's package, on a device with the feature: ours is in framework.jar, so loaded from the start. */
    public static PackageInfo getLoadedPackageInfo() {
        return isWebViewSupported() ? WebViewPackage.packageInfo() : null;
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
            if (sProviderInstance == null) {
                sProviderInstance = new WebViewFactoryProviderImpl();
            }
            return sProviderInstance;
        }
    }

    /** FindAddress for Minato's provider's statics (WebView.findAddress): the class is package-private. */
    public static String findAddress(String addr) {
        return FindAddress.findAddress(addr);
    }

    /** The update service, which only a device with the webview feature has. */
    public static IWebViewUpdateService getUpdateService() {
        return null;
    }
}
