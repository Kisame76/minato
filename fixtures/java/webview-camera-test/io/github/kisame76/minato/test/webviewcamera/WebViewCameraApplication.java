package io.github.kisame76.minato.test.webviewcamera;

import android.app.Application;
import android.content.pm.PackageManager;
import android.content.pm.PackageInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.graphics.drawable.ShapeDrawable;
import android.graphics.drawable.shapes.OvalShape;
import android.graphics.drawable.shapes.RectShape;
import android.hardware.Camera;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CaptureFailure;
import android.hardware.camera2.params.MeteringRectangle;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.print.PageRange;
import android.print.PrintAttributes;
import android.print.PrintDocumentAdapter;
import android.print.PrintDocumentInfo;
import android.print.PrintJob;
import android.net.Uri;
import android.util.Log;
import android.webkit.ClientCertRequest;
import android.webkit.ConsoleMessage;
import android.webkit.CookieManager;
import android.webkit.DownloadListener;
import android.webkit.HttpAuthHandler;
import android.webkit.JavascriptInterface;
import android.webkit.JsPromptResult;
import android.webkit.JsResult;
import android.webkit.PermissionRequest;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.SafeBrowsingResponse;
import android.webkit.ServiceWorkerClient;
import android.webkit.ServiceWorkerWebSettings;
import android.webkit.SslErrorHandler;
import android.webkit.TracingConfig;
import android.webkit.ValueCallback;
import android.webkit.WebBackForwardList;
import android.webkit.WebHistoryItem;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.webkit.WebViewRenderProcess;
import android.webkit.WebViewRenderProcessClient;

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
 * enums, the WebView class, the rest of android.webkit's public types (category 1 of the gap sort), and the camera
 * count, Camera.open(), open(0), getCameraInfo(0) and Camera.Area; then camera2's and android.print's plain types and
 * ShapeDrawable (category 1, package 2).
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
        webkitTypes();
        camera();
        camera2();
        print();
        shapes();
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

    /** A class the bridge of a page's JavaScript would call, as the ad SDKs give WebView.addJavascriptInterface. */
    public static final class Bridge {
        @JavascriptInterface
        public String version() {
            return "1";
        }
    }

    /** The rest of android.webkit's public types the ad SDKs name: the plain ones made, the abstract ones described. */
    private static void webkitTypes() {
        ConsoleMessage message = new ConsoleMessage("hello", "page.js", 7, ConsoleMessage.MessageLevel.WARNING);
        Log.i(TAG, "ConsoleMessage: " + message.message() + ", " + message.sourceId() + ":" + message.lineNumber()
                + ", " + message.messageLevel() + ", levels "
                + java.util.Arrays.toString(ConsoleMessage.MessageLevel.values()));
        TracingConfig tracing = new TracingConfig.Builder()
                .addCategories(TracingConfig.CATEGORIES_WEB_DEVELOPER).addCategories("blink*", "v8")
                .setTracingMode(TracingConfig.RECORD_CONTINUOUSLY).build();
        Log.i(TAG, "TracingConfig: categories " + tracing.getPredefinedCategories() + ", custom "
                + tracing.getCustomIncludedCategories() + ", mode " + tracing.getTracingMode());
        Log.i(TAG, "WebMessage: " + new WebMessage("data").getData() + ", ports " + new WebMessage("data").getPorts()
                + ", " + new WebMessage("d", new WebMessagePort[0]).getPorts().length);
        Log.i(TAG, "ServiceWorkerClient: shouldInterceptRequest "
                + new ServiceWorkerClient().shouldInterceptRequest(request("https://example.com/sw.js")));
        StringBuilder calls = new StringBuilder();
        ValueCallback<String> callback = value -> calls.append("value ").append(value);
        callback.onReceiveValue("v");
        DownloadListener download = (url, agent, disposition, type, length) -> calls.append(", download ").append(url)
                .append(" ").append(type).append(" ").append(length);
        download.onDownloadStart("https://example.com/f.zip", "agent", "attachment", "application/zip", 42);
        new WebMessagePort.WebMessageCallback() {}.onMessage(null, new WebMessage("m"));
        Log.i(TAG, "callbacks: " + calls + ", WebMessageCallback's default ran");
        Log.i(TAG, "JavascriptInterface: on Bridge.version " + outcome(() -> Bridge.class.getMethod("version")
                .isAnnotationPresent(JavascriptInterface.class)));
        Class<?>[] types = {ClientCertRequest.class, HttpAuthHandler.class, JsPromptResult.class, JsResult.class,
                PermissionRequest.class, RenderProcessGoneDetail.class, SafeBrowsingResponse.class,
                ServiceWorkerWebSettings.class, SslErrorHandler.class, WebBackForwardList.class, WebHistoryItem.class,
                WebMessagePort.class, WebResourceError.class, WebViewRenderProcess.class,
                WebViewRenderProcessClient.class};
        for (Class<?> type : types) {
            Log.i(TAG, "type " + type.getSimpleName() + ": abstract " + Modifier.isAbstract(type.getModifiers())
                    + ", extends " + type.getSuperclass().getName());
        }
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

    private static String describe(Class<?> type) {
        return "type " + type.getName() + ": abstract " + Modifier.isAbstract(type.getModifiers()) + ", extends "
                + type.getSuperclass().getName();
    }

    /** camera2's plain types that Unity's Camera2Wrapper names: the exception, a metering area and the callbacks. */
    private static void camera2() {
        CameraAccessException disabled = new CameraAccessException(CameraAccessException.CAMERA_DISABLED);
        CameraAccessException error = new CameraAccessException(CameraAccessException.CAMERA_ERROR, "boom");
        Log.i(TAG, "CameraAccessException: " + disabled.getReason() + " " + disabled.getMessage() + "; "
                + error.getReason() + " " + error.getMessage());
        MeteringRectangle area = new MeteringRectangle(10, 20, 30, 40, 500);
        Log.i(TAG, "MeteringRectangle: " + area + ", rect " + area.getRect().toShortString() + ", x " + area.getX()
                + " y " + area.getY() + " " + area.getWidth() + " x " + area.getHeight() + ", weight "
                + area.getMeteringWeight() + ", equals the same by rect "
                + area.equals(new MeteringRectangle(new Rect(10, 20, 40, 60), 500)));
        Log.i(TAG, "MeteringRectangle refusals: " + outcome(() -> new MeteringRectangle(1, 1, 1, 1, -1)) + "; "
                + outcome(() -> new MeteringRectangle(1, 1, 1, 1, 1001)) + "; "
                + outcome(() -> new MeteringRectangle(-1, 1, 1, 1, 0)));
        new CameraCaptureSession.CaptureCallback() {}.onCaptureStarted(null, null, 0, 0);
        for (Class<?> type : new Class<?>[] {CameraCaptureSession.class, CameraCaptureSession.CaptureCallback.class,
                CameraCaptureSession.StateCallback.class, CaptureFailure.class}) {
            Log.i(TAG, describe(type));
        }
    }

    /** android.print's plain types that the SDKs' print paths name: page ranges, a document's info and an adapter. */
    private static void print() {
        PageRange range = new PageRange(0, 4);
        Log.i(TAG, "PageRange: " + range + ", " + range.getStart() + "-" + range.getEnd() + ", all "
                + PageRange.ALL_PAGES + ", equals " + range.equals(new PageRange(0, 4)));
        Log.i(TAG, "PageRange refusals: " + outcome(() -> new PageRange(-1, 2)) + "; "
                + outcome(() -> new PageRange(3, 1)));
        PrintDocumentInfo info = new PrintDocumentInfo.Builder("report.pdf")
                .setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT).setPageCount(3).build();
        Log.i(TAG, "PrintDocumentInfo: " + info.getName() + ", type " + info.getContentType() + ", pages "
                + info.getPageCount() + ", size " + info.getDataSize() + ", " + info);
        Log.i(TAG, "PrintDocumentInfo refusals: " + outcome(() -> new PrintDocumentInfo.Builder("")) + "; "
                + outcome(() -> new PrintDocumentInfo.Builder("a").setPageCount(-2)));
        PrintDocumentAdapter adapter = new PrintDocumentAdapter() {
            @Override
            public void onLayout(PrintAttributes oldAttributes, PrintAttributes newAttributes,
                    CancellationSignal cancellationSignal, LayoutResultCallback callback, Bundle extras) {
            }

            @Override
            public void onWrite(PageRange[] pages, ParcelFileDescriptor destination,
                    CancellationSignal cancellationSignal, WriteResultCallback callback) {
            }
        };
        adapter.onStart();
        adapter.onFinish();
        for (Class<?> type : new Class<?>[] {PrintDocumentAdapter.class,
                PrintDocumentAdapter.LayoutResultCallback.class, PrintDocumentAdapter.WriteResultCallback.class,
                PrintJob.class}) {
            Log.i(TAG, describe(type));
        }
    }

    /** ShapeDrawable, drawn into a bitmap: an oval and a rectangle, their center and corner pixels. */
    private static void shapes() {
        for (ShapeDrawable shape : new ShapeDrawable[] {new ShapeDrawable(new OvalShape()),
                new ShapeDrawable(new RectShape())}) {
            shape.setIntrinsicWidth(8);
            shape.setIntrinsicHeight(8);
            shape.getPaint().setColor(0xff00ff00);
            shape.setBounds(0, 0, 8, 8);
            Bitmap bitmap = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888);
            shape.draw(new Canvas(bitmap));
            Log.i(TAG, "ShapeDrawable " + shape.getShape().getClass().getSimpleName() + ": " + shape.getIntrinsicWidth()
                    + " x " + shape.getIntrinsicHeight() + ", opacity " + shape.getOpacity() + ", alpha "
                    + shape.getAlpha() + ", center " + Integer.toHexString(bitmap.getPixel(4, 4)) + ", corner "
                    + Integer.toHexString(bitmap.getPixel(0, 0)));
        }
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
