package io.github.kisame76.minato.test.webviewlate;

import android.app.Activity;
import android.os.AsyncTask;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.ConsoleMessage;
import android.webkit.CookieManager;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * A WebView made late, as HTML5 app wrappers start: before it makes its WebView, the activity waits on each thing such
 * a wrapper waits on, one after the other: a delay, an AsyncTask that copies the page from the assets to the app's
 * files, Safe Browsing and the removal of the cookies. A step without an answer in 5 s is logged as such, and the next
 * one goes on, so one run names every wait that never ends. Then a WebView with file access off (targetSdk 35's
 * default) loads the copied page as a file:// URL and is refused, and one with file access on loads it, and its script
 * logs that it ran; the app ends with "done". The network's waits are ADR-0054 step d)'s.
 */
public class LateActivity extends Activity {
    private static final String TAG = "webview-late-test";
    private static final long LIMIT_MS = 5000;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private FrameLayout mContent;
    private int mStep;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    /** A step's answer: the first one counts, and moves on to the next step; none within LIMIT_MS is logged. */
    private final class Answer {
        private final String mName;
        private boolean mGiven;
        private final Runnable mLimit = () -> give(null);

        Answer(String name) {
            mName = name;
            mHandler.postDelayed(mLimit, LIMIT_MS);
        }

        /** Gives the answer on the main thread. */
        void post(String line) {
            mHandler.post(() -> give(line));
        }

        void give(String line) {
            if (mGiven) return;
            mGiven = true;
            mHandler.removeCallbacks(mLimit);
            log(line != null ? line : mName + ": no answer in " + LIMIT_MS / 1000 + " s");
            next();
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mContent = new FrameLayout(this);
        setContentView(mContent);
        log("created");
        next();
    }

    @SuppressWarnings("deprecation")
    private void next() {
        mStep++;
        switch (mStep) {
            case 1: {
                Answer answer = new Answer("delay");
                mHandler.postDelayed(() -> answer.give("delayed"), 300);
                break;
            }
            case 2: {
                Answer answer = new Answer("copy");
                new AsyncTask<Void, Void, Integer>() {
                    @Override
                    protected Integer doInBackground(Void... nothing) {
                        return copyPage();
                    }

                    @Override
                    protected void onPostExecute(Integer copied) {
                        answer.give("copied " + copied + " files from the assets");
                    }
                }.execute();
                break;
            }
            case 3: {
                Answer answer = new Answer("safe browsing");
                WebView.startSafeBrowsing(this, value -> answer.give("safe browsing: answered"));
                break;
            }
            case 4: {
                Answer answer = new Answer("cookies");
                CookieManager.getInstance().removeAllCookies(value -> answer.give("cookies: removed"));
                break;
            }
            case 5:
                loadWithoutFileAccess(new Answer("without file access"));
                break;
            case 6:
                makeWebView(new Answer("page"));
                break;
            default:
                log("done");
        }
    }

    /** Copies the assets' www folder to the app's files, as wrappers that serve their page from there do. */
    private int copyPage() {
        File folder = new File(getFilesDir(), "www");
        folder.mkdirs();
        int copied = 0;
        try {
            for (String name : getAssets().list("www")) {
                try (InputStream in = getAssets().open("www/" + name);
                     OutputStream out = new FileOutputStream(new File(folder, name))) {
                    byte[] buffer = new byte[8192];
                    for (int read; (read = in.read(buffer)) > 0;) out.write(buffer, 0, read);
                }
                copied++;
            }
        } catch (IOException e) {
            log("copy: " + e);
        }
        return copied;
    }

    private String page() {
        return "file://" + new File(getFilesDir(), "www/index.html").getPath();
    }

    /** The page in a WebView whose file access is off, as targetSdk 35 leaves it: the main frame's error. */
    private void loadWithoutFileAccess(Answer answer) {
        WebView view = new WebView(this);
        view.setWebViewClient(new WebViewClient() {
            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (request.isForMainFrame()) {
                    answer.give("without file access: " + error.getErrorCode() + " " + error.getDescription());
                }
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                answer.give("without file access: the page finished");
            }
        });
        mContent.addView(view);
        view.loadUrl(page());
    }

    private void makeWebView(Answer answer) {
        WebView view = new WebView(this);
        WebSettings settings = view.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setAllowFileAccess(true);
        view.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onConsoleMessage(ConsoleMessage message) {
                answer.give("page: " + message.message());
                return true;
            }
        });
        mContent.addView(view);
        log("webview made");
        view.loadUrl(page());
    }
}
