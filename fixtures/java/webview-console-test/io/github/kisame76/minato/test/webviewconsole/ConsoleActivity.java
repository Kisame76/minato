package io.github.kisame76.minato.test.webviewconsole;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.ConsoleMessage;
import android.webkit.WebChromeClient;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/**
 * A page's console messages (ADR-0056 c3): an asset page logs at each level and throws, three times. With no
 * WebChromeClient, Android's WebView logs each message under the tag chromium; with one whose onConsoleMessage returns
 * false, the client hears it and the log has it too; with one that returns true, only the client hears it. A second
 * after each page has finished, so its messages are in, the app logs the next round; it ends with "done".
 */
public class ConsoleActivity extends Activity {
    private static final String TAG = "webview-console-test";
    private static final String PAGE = "file:///android_asset/console.html";

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private WebView mView;
    private int mRound;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static final class Client extends WebChromeClient {
        private final boolean mHandled;

        Client(boolean handled) {
            mHandled = handled;
        }

        @Override
        public boolean onConsoleMessage(ConsoleMessage message) {
            log("client: " + message.messageLevel() + " " + message.message() + " at " + message.lineNumber()
                    + ", " + mHandled);
            return mHandled;
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
                mHandler.postDelayed(ConsoleActivity.this::next, 1000);
            }
        });
        log("no client");
        mView.loadUrl(PAGE);
    }

    private void next() {
        mRound++;
        if (mRound == 1) {
            log("a client that returns false");
            mView.setWebChromeClient(new Client(false));
            mView.loadUrl(PAGE + "?2");
        } else if (mRound == 2) {
            log("a client that returns true");
            mView.setWebChromeClient(new Client(true));
            mView.loadUrl(PAGE + "?3");
        } else if (mRound == 3) {
            log("done");
        }
    }
}
