package android.webkit;

import android.content.pm.PackageInfo;

/**
 * Compile-only stub (the Unity test APK probe's batch C), never in framework.jar: the WebView update service's manager, which
 * WebView.getCurrentWebViewPackage asks only with the flag update_service_ipc_wrapper, off in the release (ap3a).
 */
public final class WebViewUpdateManager {
    public static WebViewUpdateManager getInstance() {
        throw new RuntimeException("stub");
    }

    public PackageInfo getCurrentWebViewPackage() {
        throw new RuntimeException("stub");
    }
}
