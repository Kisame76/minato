package io.github.kisame76.minato.test.webviewcamera;

import android.app.Application;
import android.content.pm.PackageManager;
import android.content.pm.PackageInfo;
import android.graphics.Rect;
import android.hardware.Camera;
import android.net.Uri;
import android.util.Log;
import android.webkit.CookieManager;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.io.StringBufferInputStream;
import java.lang.reflect.Modifier;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.Callable;

/**
 * The Unity test APK probe's batch C (Grafik): android.webkit's classes, which the ad SDKs name (AppLovin's tryToCreateWebView,
 * SafeDK's brand safety, Facebook's augmentWebView), and the legacy android.hardware.Camera, whose Camera$Area Unity's
 * Camera2Wrapper names; MimeTypeMap is mime-test's. From Application.onCreate, on the main thread, it logs a
 * WebViewClient's defaults, WebResourceResponse with the arguments it refuses, a WebResourceRequest, WebSettings'
 * enums, the WebView class, and the camera count, Camera.open(), open(0), getCameraInfo(0) and Camera.Area.
 * What a WebView needs a provider for (a new WebView, CookieManager, the default user agent, the provider package and
 * the webview feature) goes to the tag webview-camera-test-device, which the check shows and does not compare: the AVD
 * has a WebView, and Minato reports none (Android 15's answers without the feature are UnsupportedOperationException,
 * and no package). Only public API, as an app has it on Android. It logs with the tag webview-camera-test and ends with
 * "done"; tools/android-reference.sh compares it with fixtures/expected/webview-camera-test.logcat.
 */
public class WebViewCameraApplication extends Application {
    private static final String TAG = "webview-camera-test";
    private static final String DEVICE_TAG = "webview-camera-test-device";

    @Override
    public void onCreate() {
        super.onCreate();
        client();
        responses();
        settings();
        camera();
        provider();
        Log.i(TAG, "done");
    }

    private static String outcome(Callable<Object> call) {
        try {
            return String.valueOf(call.call());
        } catch (Exception e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    private static WebResourceRequest request(String url) {
        return new WebResourceRequest() {
            @Override
            public Uri getUrl() {
                return Uri.parse(url);
            }

            @Override
            public boolean isForMainFrame() {
                return true;
            }

            @Override
            public boolean isRedirect() {
                return false;
            }

            @Override
            public boolean hasGesture() {
                return false;
            }

            @Override
            public String getMethod() {
                return "GET";
            }

            @Override
            public Map<String, String> getRequestHeaders() {
                return new TreeMap<>();
            }
        };
    }

    private static void client() {
        WebViewClient client = new WebViewClient();
        WebResourceRequest request = request("https://example.com/page?q=1");
        Log.i(TAG, "WebViewClient: shouldOverrideUrlLoading(url) " + client.shouldOverrideUrlLoading(null,
                "https://example.com/") + ", (request) " + client.shouldOverrideUrlLoading(null, request));
        Log.i(TAG, "WebViewClient: shouldInterceptRequest(url) " + client.shouldInterceptRequest(null,
                "https://example.com/") + ", (request) " + client.shouldInterceptRequest(null, request));
        Log.i(TAG, "WebViewClient: shouldOverrideKeyEvent " + client.shouldOverrideKeyEvent(null, null)
                + ", onRenderProcessGone " + client.onRenderProcessGone(null, null));
        Log.i(TAG, "WebResourceRequest: " + request.getUrl() + ", " + request.getMethod() + ", main frame "
                + request.isForMainFrame() + ", redirect " + request.isRedirect());
    }

    private static String describe(WebResourceResponse response) {
        return response.getMimeType() + ", " + response.getEncoding() + ", " + response.getStatusCode() + " "
                + response.getReasonPhrase() + ", headers " + response.getResponseHeaders() + ", data "
                + (response.getData() != null);
    }

    private static void responses() {
        InputStream data = new ByteArrayInputStream(new byte[] {1, 2, 3});
        WebResourceResponse plain = new WebResourceResponse("text/html", "utf-8", data);
        Log.i(TAG, "WebResourceResponse: " + describe(plain));
        Map<String, String> headers = new TreeMap<>();
        headers.put("Cache-Control", "no-cache");
        WebResourceResponse full = new WebResourceResponse("image/png", null, 404, "Not Found", headers, null);
        Log.i(TAG, "WebResourceResponse: " + describe(full));
        Object[][] codes = {{99, "x"}, {600, "x"}, {302, "Found"}, {200, null}, {200, ""}, {200, "Über"},
                {204, "No Content"}};
        for (Object[] code : codes) {
            Log.i(TAG, "setStatusCodeAndReasonPhrase(" + code[0] + ", " + code[1] + "): " + outcome(() -> {
                full.setStatusCodeAndReasonPhrase((Integer) code[0], (String) code[1]);
                return full.getStatusCode() + " " + full.getReasonPhrase();
            }));
        }
        Log.i(TAG, "setData(StringBufferInputStream): " + outcome(() -> {
            full.setData(new StringBufferInputStream("x"));
            return "set";
        }));
        full.setMimeType("text/plain");
        full.setEncoding("ascii");
        full.setResponseHeaders(null);
        full.setData(data);
        Log.i(TAG, "WebResourceResponse after the setters: " + describe(full));
    }

    private static void settings() {
        Log.i(TAG, "WebSettings: abstract " + Modifier.isAbstract(WebSettings.class.getModifiers()) + ", layout "
                + java.util.Arrays.toString(WebSettings.LayoutAlgorithm.values()) + ", plugins "
                + java.util.Arrays.toString(WebSettings.PluginState.values()) + ", cache "
                + WebSettings.LOAD_DEFAULT + " " + WebSettings.LOAD_NO_CACHE);
        Log.i(TAG, "WebView: extends " + WebView.class.getSuperclass().getName() + ", hit test types "
                + WebView.HitTestResult.UNKNOWN_TYPE + " " + WebView.HitTestResult.SRC_ANCHOR_TYPE);
    }

    private void camera() {
        Log.i(TAG, "Camera: getNumberOfCameras " + Camera.getNumberOfCameras() + ", open() " + Camera.open());
        Log.i(TAG, "Camera.open(0): " + outcome(() -> Camera.open(0)));
        Log.i(TAG, "Camera.getCameraInfo(0): " + outcome(() -> {
            Camera.CameraInfo info = new Camera.CameraInfo();
            Camera.getCameraInfo(0, info);
            return "facing " + info.facing;
        }));
        Camera.Area area = new Camera.Area(new Rect(-500, -500, 500, 500), 1000);
        Camera.Area same = new Camera.Area(new Rect(-500, -500, 500, 500), 1000);
        Camera.Area lighter = new Camera.Area(new Rect(-500, -500, 500, 500), 1);
        Log.i(TAG, "Camera.Area: " + area.rect.toShortString() + " weight " + area.weight + ", equals the same "
                + area.equals(same) + ", another weight " + area.equals(lighter) + ", null " + area.equals(null));
    }

    private void provider() {
        Log.i(DEVICE_TAG, "the webview feature: "
                + getPackageManager().hasSystemFeature(PackageManager.FEATURE_WEBVIEW));
        Log.i(DEVICE_TAG, "new WebView: " + outcome(() -> {
            WebView view = new WebView(this);
            view.destroy();
            return "made";
        }));
        Log.i(DEVICE_TAG, "CookieManager.getInstance: " + outcome(() -> CookieManager.getInstance() != null));
        Log.i(DEVICE_TAG, "WebSettings.getDefaultUserAgent: "
                + outcome(() -> WebSettings.getDefaultUserAgent(this) != null));
        Log.i(DEVICE_TAG, "WebView.getCurrentWebViewPackage: " + outcome(() -> {
            PackageInfo info = WebView.getCurrentWebViewPackage();
            return info == null ? null : "a package";
        }));
    }
}
