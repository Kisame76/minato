package com.android.internal.minato.webkit;

import android.content.Intent;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Picture;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.net.http.SslCertificate;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.print.PrintDocumentAdapter;
import android.util.Log;
import android.view.DragEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewStructure;
import android.view.WindowInsets;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.webkit.DownloadListener;
import android.webkit.ValueCallback;
import android.webkit.WebBackForwardList;
import android.webkit.WebChromeClient;
import android.webkit.WebHistoryItem;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.webkit.WebViewProvider;
import android.webkit.WebViewRenderProcess;
import android.webkit.WebViewRenderProcessClient;

import java.io.BufferedWriter;
import java.io.File;
import java.util.Map;
import java.util.concurrent.Executor;

/**
 * One WebView's provider (ADR-0056 step b1): what Chromium's WebViewChromium is to AOSP's facade, without a page yet.
 * It keeps what the app sets (settings, clients, listeners, the background color) and answers the getters as a
 * WebView that has loaded nothing does; the view lays out and takes no input. It draws as Chromium's does on Android
 * 15 (webview-start-test on the AVD): opaque black before the app asks for any content, then its background color,
 * white unless the app sets one, as an empty page shows. A load or a script is logged once per process, without its
 * URL, since the page comes with the host's WKWebView in step c); a script's callback gets "null" so that an app
 * waiting for it goes on. AOSP's facade checks the UI thread before each call.
 */
@SuppressWarnings("deprecation") // The interface's deprecated members, which Chromium's implements too.
final class WebViewProviderImpl implements WebViewProvider, WebViewProvider.ViewDelegate,
        WebViewProvider.ScrollDelegate {
    /** The one tag of the provider's notes, so that a report shows them without page data. */
    static final String TAG = "MinatoWebView";
    private static boolean sNoContentLogged;

    private final WebView mWebView;
    private final WebView.PrivateAccess mPrivate;
    private final WebSettings mSettings;
    private WebViewClient mClient = new WebViewClient();
    private WebChromeClient mChromeClient;
    private WebViewRenderProcessClient mRenderProcessClient;
    private int mBackgroundColor = Color.WHITE;
    /** The app has asked for content (a load or a script): from then on the page's area shows the background. */
    private boolean mContentRequested;
    private final Paint mBackgroundPaint = new Paint();
    private boolean mPaused;
    private int mRendererPriority = WebView.RENDERER_PRIORITY_IMPORTANT;
    private boolean mRendererWaived;

    WebViewProviderImpl(WebView webView, WebView.PrivateAccess privateAccess) {
        mWebView = webView;
        mPrivate = privateAccess;
        mSettings = new WebSettingsImpl(webView.getContext());
    }

    /** The app asked for content, which comes in step c): the area turns to the page's background. */
    private void requested() {
        if (!mContentRequested) {
            mContentRequested = true;
            mWebView.invalidate();
        }
        noContent();
    }

    /** The page this would show comes in step c): noted once per process, without what was asked. */
    private static synchronized void noContent() {
        if (!sNoContentLogged) {
            sNoContentLogged = true;
            Log.w(TAG, "web content not available yet: the WebView stays empty (ADR-0056 step c)");
        }
    }

    /** No pages, so no history: an empty list, as Chromium's before the first load. */
    @SuppressWarnings("serial")
    private static final class EmptyHistory extends WebBackForwardList {
        @Override public WebHistoryItem getCurrentItem() { return null; }
        @Override public int getCurrentIndex() { return -1; }
        @Override public WebHistoryItem getItemAtIndex(int index) { return null; }
        @Override public int getSize() { return 0; }
        @Override protected WebBackForwardList clone() { return new EmptyHistory(); }
    }

    // WebViewProvider

    @Override
    public void init(Map<String, Object> javaScriptInterfaces, boolean privateBrowsing) {
        if (privateBrowsing && mWebView.getContext().getApplicationInfo().targetSdkVersion
                >= Build.VERSION_CODES.KITKAT) {
            throw new IllegalArgumentException("Private browsing is not supported in WebView.");
        }
    }

    @Override public void setHorizontalScrollbarOverlay(boolean overlay) {}
    @Override public void setVerticalScrollbarOverlay(boolean overlay) {}
    @Override public boolean overlayHorizontalScrollbar() { return true; }
    @Override public boolean overlayVerticalScrollbar() { return false; }
    @Override public int getVisibleTitleHeight() { return 0; }
    @Override public SslCertificate getCertificate() { return null; }
    @Override public void setCertificate(SslCertificate certificate) {}
    @Override public void savePassword(String host, String username, String password) {}
    @Override public void setHttpAuthUsernamePassword(String host, String realm, String username, String password) {}
    @Override public String[] getHttpAuthUsernamePassword(String host, String realm) { return null; }
    @Override public void destroy() {}
    @Override public void setNetworkAvailable(boolean networkUp) {}
    @Override public WebBackForwardList saveState(Bundle outState) { return null; }
    @Override public boolean savePicture(Bundle b, File dest) { return false; }
    @Override public boolean restorePicture(Bundle b, File src) { return false; }
    @Override public WebBackForwardList restoreState(Bundle inState) { return null; }
    @Override public void loadUrl(String url, Map<String, String> additionalHttpHeaders) { requested(); }
    @Override public void loadUrl(String url) { requested(); }
    @Override public void postUrl(String url, byte[] postData) { requested(); }
    @Override public void loadData(String data, String mimeType, String encoding) { requested(); }

    @Override
    public void loadDataWithBaseURL(String baseUrl, String data, String mimeType, String encoding,
            String historyUrl) {
        requested();
    }

    @Override
    public void evaluateJavaScript(String script, ValueCallback<String> resultCallback) {
        requested();
        if (resultCallback != null) {
            new Handler(Looper.getMainLooper()).post(() -> resultCallback.onReceiveValue("null"));
        }
    }

    @Override public void saveWebArchive(String filename) {}

    @Override
    public void saveWebArchive(String basename, boolean autoname, ValueCallback<String> callback) {
        if (callback != null) {
            new Handler(Looper.getMainLooper()).post(() -> callback.onReceiveValue(null));
        }
    }

    @Override public void stopLoading() {}
    @Override public void reload() {}
    @Override public boolean canGoBack() { return false; }
    @Override public void goBack() {}
    @Override public boolean canGoForward() { return false; }
    @Override public void goForward() {}
    @Override public boolean canGoBackOrForward(int steps) { return steps == 0; }
    @Override public void goBackOrForward(int steps) {}
    @Override public boolean isPrivateBrowsingEnabled() { return false; }
    @Override public boolean pageUp(boolean top) { return false; }
    @Override public boolean pageDown(boolean bottom) { return false; }

    @Override
    public void insertVisualStateCallback(long requestId, WebView.VisualStateCallback callback) {
        new Handler(Looper.getMainLooper()).post(() -> callback.onComplete(requestId));
    }

    @Override public void clearView() {}
    @Override public Picture capturePicture() { return new Picture(); }
    @Override public PrintDocumentAdapter createPrintDocumentAdapter(String documentName) { return null; }
    @Override public float getScale() { return mWebView.getResources().getDisplayMetrics().density; }
    @Override public void setInitialScale(int scaleInPercent) {}
    @Override public void invokeZoomPicker() {}
    @Override public WebView.HitTestResult getHitTestResult() { return new WebView.HitTestResult(); }
    @Override public void requestFocusNodeHref(Message hrefMsg) {}
    @Override public void requestImageRef(Message msg) {}
    @Override public String getUrl() { return null; }
    @Override public String getOriginalUrl() { return null; }
    /** As Chromium's for a WebView that has shown no page: an empty title, where the URL is null. */
    @Override public String getTitle() { return ""; }
    @Override public Bitmap getFavicon() { return null; }
    @Override public String getTouchIconUrl() { return null; }
    @Override public int getProgress() { return 100; }
    @Override public int getContentHeight() { return 0; }
    @Override public int getContentWidth() { return 0; }
    @Override public void pauseTimers() {}
    @Override public void resumeTimers() {}
    @Override public void onPause() { mPaused = true; }
    @Override public void onResume() { mPaused = false; }
    @Override public boolean isPaused() { return mPaused; }
    @Override public void freeMemory() {}
    @Override public void clearCache(boolean includeDiskFiles) {}
    @Override public void clearFormData() {}
    @Override public void clearHistory() {}
    @Override public void clearSslPreferences() {}
    @Override public WebBackForwardList copyBackForwardList() { return new EmptyHistory(); }
    @Override public void setFindListener(WebView.FindListener listener) {}
    @Override public void findNext(boolean forward) {}
    @Override public int findAll(String find) { return 0; }
    @Override public void findAllAsync(String find) {}
    @Override public boolean showFindDialog(String text, boolean showIme) { return false; }
    @Override public void clearMatches() {}

    @Override
    public void documentHasImages(Message response) {
        response.arg1 = 0;
        response.sendToTarget();
    }

    @Override public void setWebViewClient(WebViewClient client) { mClient = client; }
    @Override public WebViewClient getWebViewClient() { return mClient; }
    @Override public WebViewRenderProcess getWebViewRenderProcess() { return null; }

    @Override
    public void setWebViewRenderProcessClient(Executor executor, WebViewRenderProcessClient client) {
        mRenderProcessClient = client;
    }

    @Override public WebViewRenderProcessClient getWebViewRenderProcessClient() { return mRenderProcessClient; }
    @Override public void setDownloadListener(DownloadListener listener) {}
    @Override public void setWebChromeClient(WebChromeClient client) { mChromeClient = client; }
    @Override public WebChromeClient getWebChromeClient() { return mChromeClient; }
    @Override public void setPictureListener(WebView.PictureListener listener) {}
    @Override public void addJavascriptInterface(Object obj, String interfaceName) {}
    @Override public void removeJavascriptInterface(String interfaceName) {}
    @Override public WebMessagePort[] createWebMessageChannel() { return new WebMessagePort[0]; }
    @Override public void postMessageToMainFrame(WebMessage message, Uri targetOrigin) {}
    @Override public WebSettings getSettings() { return mSettings; }
    @Override public void setMapTrackballToArrowKeys(boolean setMap) {}
    @Override public void flingScroll(int vx, int vy) {}
    @Override public View getZoomControls() { return null; }
    @Override public boolean canZoomIn() { return false; }
    @Override public boolean canZoomOut() { return false; }
    @Override public boolean zoomBy(float zoomFactor) { return false; }
    @Override public boolean zoomIn() { return false; }
    @Override public boolean zoomOut() { return false; }
    @Override public void dumpViewHierarchyWithProperties(BufferedWriter out, int level) {}
    @Override public View findHierarchyView(String className, int hashCode) { return null; }

    @Override
    public void setRendererPriorityPolicy(int rendererRequestedPriority, boolean waivedWhenNotVisible) {
        mRendererPriority = rendererRequestedPriority;
        mRendererWaived = waivedWhenNotVisible;
    }

    @Override public int getRendererRequestedPriority() { return mRendererPriority; }
    @Override public boolean getRendererPriorityWaivedWhenNotVisible() { return mRendererWaived; }
    @Override public ViewDelegate getViewDelegate() { return this; }
    @Override public ScrollDelegate getScrollDelegate() { return this; }
    @Override public void notifyFindDialogDismissed() {}

    // ViewDelegate

    @Override public boolean shouldDelayChildPressedState() { return true; }
    @Override public void onProvideVirtualStructure(ViewStructure structure) {}
    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider() { return null; }
    @Override public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {}
    @Override public void onInitializeAccessibilityEvent(AccessibilityEvent event) {}

    @Override
    public boolean performAccessibilityAction(int action, Bundle arguments) {
        return mPrivate.super_performAccessibilityAction(action, arguments);
    }

    @Override public void setOverScrollMode(int mode) {}
    @Override public void setScrollBarStyle(int style) {}

    @Override
    public void onDrawVerticalScrollBar(Canvas canvas, Drawable scrollBar, int l, int t, int r, int b) {
        mPrivate.super_onDrawVerticalScrollBar(canvas, scrollBar, l, t, r, b);
    }

    @Override public void onOverScrolled(int scrollX, int scrollY, boolean clampedX, boolean clampedY) {}
    @Override public void onWindowVisibilityChanged(int visibility) {}

    /** The page's area, empty: black before any content was asked for, then the background color. */
    @Override
    public void onDraw(Canvas canvas) {
        int color = mContentRequested ? mBackgroundColor : Color.BLACK;
        if (Color.alpha(color) != 0) {
            mBackgroundPaint.setColor(color);
            canvas.drawRect(mWebView.getScrollX(), mWebView.getScrollY(),
                    mWebView.getScrollX() + mWebView.getWidth(), mWebView.getScrollY() + mWebView.getHeight(),
                    mBackgroundPaint);
        }
    }

    @Override
    public void setLayoutParams(ViewGroup.LayoutParams layoutParams) {
        mPrivate.super_setLayoutParams(layoutParams);
    }

    @Override public boolean performLongClick() { return mPrivate.super_performLongClick(); }
    @Override public void onConfigurationChanged(Configuration newConfig) {}
    @Override public InputConnection onCreateInputConnection(EditorInfo outAttrs) { return null; }
    @Override public boolean onDragEvent(DragEvent event) { return false; }
    @Override public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) { return false; }
    @Override public boolean onKeyDown(int keyCode, KeyEvent event) { return false; }
    @Override public boolean onKeyUp(int keyCode, KeyEvent event) { return false; }
    @Override public void onAttachedToWindow() {}
    @Override public void onDetachedFromWindow() {}
    @Override public void onVisibilityChanged(View changedView, int visibility) {}
    @Override public void onWindowFocusChanged(boolean hasWindowFocus) {}
    @Override public void onFocusChanged(boolean focused, int direction, Rect previouslyFocusedRect) {}

    @Override
    public boolean setFrame(int left, int top, int right, int bottom) {
        return mPrivate.super_setFrame(left, top, right, bottom);
    }

    @Override public void onSizeChanged(int w, int h, int ow, int oh) {}
    @Override public void onScrollChanged(int l, int t, int oldl, int oldt) {}
    @Override public boolean dispatchKeyEvent(KeyEvent event) { return mPrivate.super_dispatchKeyEvent(event); }
    @Override public boolean onTouchEvent(MotionEvent ev) { return false; }
    @Override public boolean onHoverEvent(MotionEvent event) { return mPrivate.super_onHoverEvent(event); }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        return mPrivate.super_onGenericMotionEvent(event);
    }

    @Override public boolean onTrackballEvent(MotionEvent ev) { return false; }

    @Override
    public boolean requestFocus(int direction, Rect previouslyFocusedRect) {
        return mPrivate.super_requestFocus(direction, previouslyFocusedRect);
    }

    /**
     * As Chromium's AwLayoutSizer with no content: the width the parent offers, and a height that is the parent's
     * exact one, else the content's, which is 0 while there is no page.
     */
    @Override
    public void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = View.MeasureSpec.getMode(widthMeasureSpec) == View.MeasureSpec.UNSPECIFIED ? 0
                : View.MeasureSpec.getSize(widthMeasureSpec);
        int height = View.MeasureSpec.getMode(heightMeasureSpec) == View.MeasureSpec.EXACTLY
                ? View.MeasureSpec.getSize(heightMeasureSpec) : 0;
        mPrivate.setMeasuredDimension(width, height);
    }

    @Override public boolean requestChildRectangleOnScreen(View child, Rect rect, boolean immediate) { return false; }

    @Override
    public void setBackgroundColor(int color) {
        mBackgroundColor = color;
        mWebView.invalidate();
    }

    @Override public void setLayerType(int layerType, Paint paint) {}
    @Override public void preDispatchDraw(Canvas canvas) {}
    @Override public void onStartTemporaryDetach() {}
    @Override public void onFinishTemporaryDetach() {}
    @Override public void onActivityResult(int requestCode, int resultCode, Intent data) {}
    @Override public Handler getHandler(Handler originalHandler) { return originalHandler; }
    @Override public View findFocus(View originalFocusedView) { return originalFocusedView; }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        return mPrivate.super_onApplyWindowInsets(insets);
    }

    // ScrollDelegate: no content, so the view's own size and no offset.

    @Override public int computeHorizontalScrollRange() { return mWebView.getWidth(); }
    @Override public int computeHorizontalScrollOffset() { return 0; }
    @Override public int computeVerticalScrollRange() { return mWebView.getHeight(); }
    @Override public int computeVerticalScrollOffset() { return 0; }
    @Override public int computeVerticalScrollExtent() { return mWebView.getHeight(); }
    @Override public void computeScroll() {}
}
