package android.webkit;

import android.content.pm.PackageInfo;
import android.os.RemoteException;

/**
 * Compile-only stub (the Unity test APK probe's batch C), never in framework.jar: the WebView update service's AIDL interface,
 * which WebView.getCurrentWebViewPackage asks when Minato's WebViewFactory gives one, and it gives none (no webview
 * feature).
 */
public interface IWebViewUpdateService extends android.os.IInterface {
    PackageInfo getCurrentWebViewPackage() throws RemoteException;
}
