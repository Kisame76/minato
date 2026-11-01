package io.github.kisame76.minato.test.webviewstart;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.ColorDrawable;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.ViewTreeObserver;
import android.webkit.CookieManager;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;

import java.util.concurrent.Callable;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * ADR-0056 step b1's test: a WebView made as the activity's content, as an app makes one at its start (a Compose
 * AndroidView does the same through its factory). It logs what the WebView answers before any page loads: its
 * settings' defaults, its user agent's shape (the model and the build's ID are the device's, so only their place is
 * checked), the WebView package as WebView and PackageManager give it (its name is the device's: the emulator's
 * AOSP image has com.android.webview, a phone com.google.android.webview), the cookie store and the view's getters.
 * Then how it draws in software, over a window of its own color: at its first pre-draw, before any load, and a
 * second after it loads an empty page; the reference script also takes the screen after "done". A device that cannot make one logs
 * the exception, then "done".
 */
public class WebViewStartActivity extends Activity {
    private static final String TAG = "webview-start-test";
    /** The device's own answer, shown and not compared: the WebView package's name. */
    private static final String DEVICE_TAG = "webview-start-test-device";
    /** The window's background, which shows where the empty WebView draws nothing (reference: 0xff336699). */
    private static final int WINDOW_COLOR = 0xff336699;
    /** Long enough for an empty page from loadData to show on the emulator. */
    private static final long LOAD_WAIT_MS = 1000;

    private static String outcome(Callable<Object> call) {
        try {
            return String.valueOf(call.call());
        } catch (Exception e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setBackgroundDrawable(new ColorDrawable(WINDOW_COLOR));
        log("the webview feature: " + getPackageManager().hasSystemFeature(PackageManager.FEATURE_WEBVIEW));
        final WebView view;
        try {
            view = new WebView(this);
        } catch (RuntimeException e) {
            log("new WebView: " + e.getClass().getName() + ": " + e.getMessage());
            log("done");
            return;
        }
        log("new WebView: made");
        setContentView(view);
        settings(view.getSettings());
        userAgent(view.getSettings());
        webViewPackage();
        cookies(view);
        getters(view);
        view.getViewTreeObserver().addOnPreDrawListener(new ViewTreeObserver.OnPreDrawListener() {
            @Override
            public boolean onPreDraw() {
                view.getViewTreeObserver().removeOnPreDrawListener(this);
                drawn(view, "before a load");
                view.loadData("<html><body></body></html>", "text/html", null);
                // A second request for content, so that a device's note of it shows once per process.
                view.evaluateJavascript("1", null);
                view.postDelayed(() -> {
                    drawn(view, "a second after loadData");
                    log("done");
                }, LOAD_WAIT_MS);
                return true;
            }
        });
    }

    @SuppressWarnings("deprecation")
    private void settings(WebSettings s) {
        log("settings: javaScript " + s.getJavaScriptEnabled() + ", domStorage " + s.getDomStorageEnabled()
                + ", database " + s.getDatabaseEnabled() + ", fileAccess " + s.getAllowFileAccess()
                + ", contentAccess " + s.getAllowContentAccess());
        log("settings: loadsImages " + s.getLoadsImagesAutomatically() + ", blockNetworkImage "
                + s.getBlockNetworkImage() + ", blockNetworkLoads " + s.getBlockNetworkLoads() + ", mixedContent "
                + s.getMixedContentMode() + ", cacheMode " + s.getCacheMode() + ", safeBrowsing "
                + s.getSafeBrowsingEnabled());
        log("settings: textZoom " + s.getTextZoom() + ", defaultFontSize " + s.getDefaultFontSize()
                + ", minimumFontSize " + s.getMinimumFontSize() + ", encoding " + s.getDefaultTextEncodingName()
                + ", standardFont " + s.getStandardFontFamily() + ", layout " + s.getLayoutAlgorithm());
        log("settings: wideViewPort " + s.getUseWideViewPort() + ", overview " + s.getLoadWithOverviewMode()
                + ", supportZoom " + s.supportZoom() + ", builtInZoom " + s.getBuiltInZoomControls()
                + ", displayZoom " + s.getDisplayZoomControls() + ", multipleWindows " + s.supportMultipleWindows()
                + ", jsOpensWindows " + s.getJavaScriptCanOpenWindowsAutomatically());
        log("settings: mediaNeedsGesture " + s.getMediaPlaybackRequiresUserGesture() + ", offscreenPreRaster "
                + s.getOffscreenPreRaster() + ", disabledActionModeItems " + s.getDisabledActionModeMenuItems()
                + ", forceDark " + s.getForceDark());
    }

    private void userAgent(WebSettings s) {
        String ua = s.getUserAgentString();
        log("user agent: default " + ua.equals(WebSettings.getDefaultUserAgent(this)) + ", Mozilla/5.0 (Linux; Android "
                + ua.startsWith("Mozilla/5.0 (Linux; Android " + Build.VERSION.RELEASE + "; ") + ", model and build "
                + ua.contains("; " + Build.MODEL + " Build/" + Build.ID + "; wv) ") + ", engine "
                + ua.contains("; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/") + ", end "
                + ua.endsWith(" Mobile Safari/537.36"));
        Matcher chrome = Pattern.compile(" Chrome/(\\S+) ").matcher(ua);
        String version = chrome.find() ? chrome.group(1) : null;
        PackageInfo info = WebView.getCurrentWebViewPackage();
        log("user agent: Chrome/" + version + ", the package's versionName "
                + (info != null && info.versionName.equals(version)));
    }

    private void webViewPackage() {
        PackageInfo current = WebView.getCurrentWebViewPackage();
        if (current == null) {
            log("WebView.getCurrentWebViewPackage: null");
            return;
        }
        Log.i(DEVICE_TAG, "WebView package: " + current.packageName);
        log("WebView.getCurrentWebViewPackage: " + current.versionName + " (" + current.getLongVersionCode()
                + "), enabled " + current.applicationInfo.enabled);
        PackageManager pm = getPackageManager();
        log("getPackageInfo: " + outcome(() -> {
            PackageInfo info = pm.getPackageInfo(current.packageName, 0);
            return info.versionName + " (" + info.getLongVersionCode() + ")";
        }));
        log("getApplicationInfo: " + outcome(() -> {
            ApplicationInfo info = pm.getApplicationInfo(current.packageName, 0);
            return "enabled " + info.enabled + ", system " + ((info.flags & ApplicationInfo.FLAG_SYSTEM) != 0);
        }));
    }

    private void cookies(WebView view) {
        CookieManager cookies = CookieManager.getInstance();
        log("cookies: accept " + cookies.acceptCookie() + ", thirdParty " + cookies.acceptThirdPartyCookies(view)
                + ", has " + cookies.hasCookies() + ", example.com " + cookies.getCookie("https://example.com/"));
    }

    private void getters(WebView view) {
        log("view: url " + view.getUrl() + ", original " + view.getOriginalUrl() + ", title " + view.getTitle()
                + ", progress " + view.getProgress() + ", back " + view.canGoBack() + ", forward "
                + view.canGoForward() + ", history " + view.copyBackForwardList().getSize());
        WebChromeClient chrome = view.getWebChromeClient();
        log("view: client " + view.getWebViewClient().getClass().getName() + ", chrome client "
                + (chrome == null ? null : chrome.getClass().getName()) + ", background " + view.getBackground());
    }

    /** The view in software, as a window's snapshot draws it: the color at its center. */
    private void drawn(WebView view, String when) {
        int width = view.getWidth();
        int height = view.getHeight();
        if (width <= 0 || height <= 0) {
            log("drawn " + when + ": no size");
            return;
        }
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        view.draw(new Canvas(bitmap));
        log("drawn " + when + ": center " + String.format("%08x", bitmap.getPixel(width / 2, height / 2)));
        bitmap.recycle();
    }
}
