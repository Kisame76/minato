package io.github.kisame76.minato.test.webview;

import android.app.Activity;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.util.Log;
import android.webkit.JsResult;
import android.webkit.WebBackForwardList;
import android.webkit.WebChromeClient;
import android.webkit.WebHistoryItem;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/**
 * ADR-0056 step b2's test (the ADR's test 1, its guest half): a WebView loads three pages one after another and logs
 * what its WebViewClient and WebChromeClient hear, in order. First the asset page.html, whose script shows an alert
 * while it loads; then two scripts, one that adds and one that reads the title; then a missing asset, which fails and
 * shows the engine's error page, which the next load waits a second for; then a page from loadDataWithBaseURL without
 * a base URL (an http one would fetch a favicon) a little over 1.5 MiB, whose title stands after its first MiB, so that
 * its title shows that the whole page arrived; then the history. A load reports its progress of 100 once, however often the
 * engine does. A stage that hears nothing in 10 s logs where it stalled, then "done".
 */
public class WebViewActivity extends Activity {
    private static final String TAG = "webview-test";
    private static final String PAGE = "file:///android_asset/page.html";
    private static final String MISSING = "file:///android_asset/missing.html";
    /** The filler before the big page's title: more than one record of the channel (1 MiB). */
    private static final int BIG_FILLER = 1536 * 1024;
    private static final long STALL_MS = 10000;
    /** After a failed load the engine shows its error page; the next load waits for it. */
    private static final long ERROR_PAGE_MS = 1000;

    private WebView mView;
    private int mStage;
    private boolean mFullProgress;
    private final Runnable mStalled = () -> {
        log("stalled at stage " + mStage);
        log("done");
    };

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mView = new WebView(this);
        setContentView(mView);
        mView.getSettings().setJavaScriptEnabled(true);
        mView.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageStarted(WebView view, String url, Bitmap favicon) {
                log("page started: " + url);
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                log("page finished: " + url);
                next();
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                log("error: main frame " + request.isForMainFrame() + ", code " + error.getErrorCode() + ", "
                        + error.getDescription() + ", " + request.getUrl());
            }
        });
        mView.setWebChromeClient(new WebChromeClient() {
            @Override
            public void onProgressChanged(WebView view, int progress) {
                if (progress == 100 && !mFullProgress) {
                    mFullProgress = true;
                    log("progress 100");
                }
            }

            @Override
            public void onReceivedTitle(WebView view, String title) {
                log("title: " + title);
            }

            @Override
            public boolean onJsAlert(WebView view, String url, String message, JsResult result) {
                log("alert: " + message + ", from " + url);
                result.confirm();
                return true;
            }
        });
        load(() -> mView.loadUrl(PAGE));
    }

    /** Starts the next load, and the watch over it. */
    private void load(Runnable start) {
        mStage++;
        mFullProgress = false;
        mView.removeCallbacks(mStalled);
        mView.postDelayed(mStalled, STALL_MS);
        start.run();
    }

    /** What follows the end of each stage's load. */
    private void next() {
        switch (mStage) {
            case 1:
                view("page");
                mView.evaluateJavascript("1 + 2", sum -> {
                    log("script 1 + 2: " + sum);
                    mView.evaluateJavascript("document.title", title -> {
                        log("script document.title: " + title);
                        load(() -> mView.loadUrl(MISSING));
                    });
                });
                break;
            case 2:
                view("missing, finished");
                mView.postDelayed(() -> {
                    view("missing, a second later");
                    load(() -> mView.loadDataWithBaseURL(null, bigPage(), "text/html", "utf-8", null));
                }, ERROR_PAGE_MS);
                break;
            case 3:
                view("big");
                history();
                mView.removeCallbacks(mStalled);
                log("done");
                break;
            default:
                log("page finished again at stage " + mStage);
        }
    }

    private void view(String name) {
        log(name + ": url " + mView.getUrl() + ", original " + mView.getOriginalUrl() + ", title " + mView.getTitle()
                + ", progress " + mView.getProgress());
    }

    private void history() {
        WebBackForwardList list = mView.copyBackForwardList();
        log("history: back " + mView.canGoBack() + ", forward " + mView.canGoForward() + ", size " + list.getSize()
                + ", current " + list.getCurrentIndex());
        for (int i = 0; i < list.getSize(); i++) {
            WebHistoryItem item = list.getItemAtIndex(i);
            log("history " + i + ": " + item.getUrl() + ", title " + item.getTitle());
        }
    }

    private static String bigPage() {
        StringBuilder page = new StringBuilder(BIG_FILLER + 128);
        page.append("<!DOCTYPE html><html><head><!--");
        while (page.length() < BIG_FILLER) {
            page.append("0123456789abcdef");
        }
        page.append("--><title>Big page</title></head><body>big</body></html>");
        return page.toString();
    }
}
