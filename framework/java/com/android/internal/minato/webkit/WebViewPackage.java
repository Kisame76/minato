package com.android.internal.minato.webkit;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.os.Build;

/**
 * The WebView package an app sees (ADR-0056 decision 1): a phone's com.google.android.webview, at the version of the
 * reference emulator's WebView (Android 15's image, Chromium 124.0.6367.219, versionCode 636771901), so that the user
 * agent's Chrome/ version, WebView.getCurrentWebViewPackage and the package manager agree, as they do on a phone. Ad
 * SDKs and WebView-version checks read them.
 */
public final class WebViewPackage {
    public static final String PACKAGE_NAME = "com.google.android.webview";
    static final String VERSION_NAME = "124.0.6367.219";
    static final long VERSION_CODE = 636771901L;

    private WebViewPackage() {}

    /** Android 15's WebView user agent (ADR-0056 decision 8), with this device's model and build. */
    public static String userAgent() {
        return "Mozilla/5.0 (Linux; Android " + Build.VERSION.RELEASE + "; " + Build.MODEL + " Build/" + Build.ID
                + "; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/" + VERSION_NAME
                + " Mobile Safari/537.36";
    }

    /** As a phone's: a system app, installed and enabled. */
    public static ApplicationInfo applicationInfo() {
        ApplicationInfo info = new ApplicationInfo();
        info.packageName = PACKAGE_NAME;
        info.processName = PACKAGE_NAME;
        info.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED | ApplicationInfo.FLAG_HAS_CODE;
        info.enabled = true;
        info.sourceDir = "/product/app/WebViewGoogle/WebViewGoogle.apk";
        info.publicSourceDir = info.sourceDir;
        info.targetSdkVersion = Build.VERSION_CODES.UPSIDE_DOWN_CAKE;
        info.minSdkVersion = Build.VERSION_CODES.Q;
        info.nonLocalizedLabel = "Android System WebView";
        return info;
    }

    public static PackageInfo packageInfo() {
        PackageInfo info = new PackageInfo();
        info.packageName = PACKAGE_NAME;
        info.versionName = VERSION_NAME;
        info.setLongVersionCode(VERSION_CODE);
        info.applicationInfo = applicationInfo();
        return info;
    }
}
