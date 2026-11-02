package io.github.kisame76.minato.test.webviewbridge;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/**
 * A page calls the app (ADR-0056 c3): an object added with addJavascriptInterface as "bridge" before the first load,
 * whose @JavascriptInterface methods the page calls and gets their values from at once, on a thread that is not the
 * main one; its method without the annotation the page does not see. A second object, "late", added after the first
 * page has finished, the page sees only from the next load on, and "bridge", removed, is gone only after a load too.
 * A second after each page has finished the app logs the next round; it ends with "done".
 */
public class BridgeActivity extends Activity {
    private static final String TAG = "webview-bridge-test";
    private static final String PAGE = "file:///android_asset/bridge.html";

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private WebView mView;
    private int mRound;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    /** What the page calls. */
    public static final class Bridge {
        private final String mName;

        Bridge(String name) {
            mName = name;
        }

        @JavascriptInterface
        public void note(String text) {
            log(mName + ": " + text);
        }

        @JavascriptInterface
        public String hello(String who) {
            return "hello " + who + ", on the main thread: " + (Looper.myLooper() == Looper.getMainLooper());
        }

        @JavascriptInterface
        public int add(int a, int b) {
            return a + b;
        }

        @JavascriptInterface
        public double half(double value) {
            return value / 2;
        }

        @JavascriptInterface
        public boolean not(boolean value) {
            return !value;
        }

        @JavascriptInterface
        public void nothing() {}

        public String hidden() {
            return "hidden";
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mView = new WebView(this);
        setContentView(mView);
        mView.getSettings().setJavaScriptEnabled(true);
        mView.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                mHandler.postDelayed(BridgeActivity.this::next, 1000);
            }
        });
        mView.addJavascriptInterface(new Bridge("bridge"), "bridge");
        log("bridge added");
        mView.loadUrl(PAGE);
    }

    private void next() {
        mRound++;
        if (mRound == 1) {
            mView.addJavascriptInterface(new Bridge("late"), "late");
            mView.evaluateJavascript("typeof window.late", value -> {
                log("late added, before a load: " + value);
                mView.loadUrl(PAGE + "?2");
            });
        } else if (mRound == 2) {
            mView.removeJavascriptInterface("bridge");
            mView.evaluateJavascript("typeof window.bridge", value -> {
                log("bridge removed, before a load: " + value);
                mView.loadUrl(PAGE + "?3");
            });
        } else if (mRound == 3) {
            log("done");
        }
    }
}
