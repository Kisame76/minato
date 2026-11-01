package io.github.kisame76.minato.test.webviewtouch;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.webkit.WebChromeClient;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.FrameLayout;

/**
 * ADR-0056 c2's test 3: touches on a WebView. An ad-like translucent activity shows a WebView over its whole window,
 * an asset page whose script sets the title when it is tapped, scrolled or its text field focused, and three views
 * after it: a close Button over the page's top right, which takes its taps; a view that is only focusable over the
 * tap area's left, which lets them through to the page, as View.onTouchEvent does; and a clickable view that is GONE
 * over the text field. Once the page has finished it logs where to touch, in the screen's pixels, as device lines
 * (the tag webview-touch-test-device), then "ready"; the touches, in this order, are a tap on the Button, a tap on the
 * tap point, a swipe up from the swipe line's first point to its second, and, once the app has scrolled the page back
 * to its top ("scrolled back"), so the field is where its point says however far the swipe went, a tap on the field.
 * It logs each one's effect, then "done"; with nothing new for 60 s it logs where it stalled, then "done".
 */
public class TouchActivity extends Activity {
    private static final String TAG = "webview-touch-test";
    private static final String DEVICE = TAG + "-device";
    private static final String PAGE = "file:///android_asset/touch.html";
    private static final long STALL_MS = 60000;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private WebView mView;
    private Button mClose;
    private View mFocusable;
    private boolean mReady;
    private String mWaiting = "the page";
    private final Runnable mStalled = () -> {
        log("stalled at " + mWaiting);
        log("done");
    };

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mView = new WebView(this);
        mClose = new Button(this);
        mClose.setText("Close");
        mClose.setOnClickListener(v -> {
            log("close tapped");
            waitFor("a tap on the page");
        });
        mFocusable = new View(this);
        mFocusable.setFocusable(true);
        View gone = new View(this);
        gone.setClickable(true);
        gone.setVisibility(View.GONE);
        FrameLayout root = new FrameLayout(this) {
            @Override
            protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
                super.onLayout(changed, left, top, right, bottom);
                // The page's tap area is its top 40 %, its field the next 10 %; in the window's pixels.
                int width = right - left;
                int height = bottom - top;
                mClose.layout(width * 6 / 10, 0, width, height * 12 / 100);
                mFocusable.layout(0, height * 15 / 100, width / 2, height * 35 / 100);
                gone.layout(0, height * 40 / 100, width, height * 50 / 100);
            }
        };
        root.addView(mView);
        root.addView(mClose);
        root.addView(mFocusable);
        root.addView(gone);
        setContentView(root);

        mView.getSettings().setJavaScriptEnabled(true);
        mView.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                log("page finished: " + url);
                // After the next layout, so the points are the views' last.
                mView.post(TouchActivity.this::ready);
            }
        });
        mView.setWebChromeClient(new WebChromeClient() {
            @Override
            public void onReceivedTitle(WebView view, String title) {
                if (!mReady) {
                    return;
                }
                switch (title) {
                    case "page tapped":
                        log("title: " + title);
                        waitFor("a swipe on the page");
                        break;
                    case "page scrolled":
                        log("title: " + title);
                        // A second for the fling to end, then back to the top.
                        mHandler.postDelayed(() -> mView.evaluateJavascript("window.scrollTo(0, 0); 0", v -> {
                            log("scrolled back");
                            waitFor("a tap on the field");
                        }), 1000);
                        break;
                    case "field focused":
                        log("title: " + title);
                        mHandler.removeCallbacks(mStalled);
                        log("done");
                        break;
                    default:
                        break;
                }
            }
        });
        mHandler.postDelayed(mStalled, STALL_MS);
        mView.loadUrl(PAGE);
    }

    /** Logs where to touch, in the screen's pixels, then "ready". */
    private void ready() {
        if (mReady) {
            return;
        }
        mReady = true;
        int[] at = new int[2];
        mClose.getLocationOnScreen(at);
        Log.i(DEVICE, "button point " + (at[0] + mClose.getWidth() / 2) + " " + (at[1] + mClose.getHeight() / 2));
        mFocusable.getLocationOnScreen(at);
        Log.i(DEVICE, "tap point " + (at[0] + mFocusable.getWidth() / 2) + " "
                + (at[1] + mFocusable.getHeight() / 2));
        mView.getLocationOnScreen(at);
        int width = mView.getWidth();
        int height = mView.getHeight();
        Log.i(DEVICE, "field point " + (at[0] + width / 2) + " " + (at[1] + height * 45 / 100));
        Log.i(DEVICE, "swipe line " + (at[0] + width / 2) + " " + (at[1] + height * 85 / 100) + " "
                + (at[0] + width / 2) + " " + (at[1] + height * 55 / 100));
        log("ready");
        waitFor("a tap on the Button");
    }

    private void waitFor(String what) {
        mWaiting = what;
        mHandler.removeCallbacks(mStalled);
        mHandler.postDelayed(mStalled, STALL_MS);
    }
}
