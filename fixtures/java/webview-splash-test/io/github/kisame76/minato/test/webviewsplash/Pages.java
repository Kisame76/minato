package io.github.kisame76.minato.test.webviewsplash;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/** The page both activities load, and what each logs once it has finished and the view had a frame to show it. */
final class Pages {
    static final String TAG = "webview-splash-test";
    static final String PAGE = "file:///android_asset/page.html";

    private Pages() {}

    static void log(String line) {
        Log.i(TAG, line);
    }

    /** Loads the page into `view`; `then` runs once the page has finished and its state was read. */
    static void load(String who, WebView view, Runnable then) {
        Handler handler = new Handler(Looper.getMainLooper());
        Runnable timeout = () -> {
            log(who + ": timeout, the page did not finish");
            then.run();
        };
        handler.postDelayed(timeout, 10000);
        view.getSettings().setJavaScriptEnabled(true);
        view.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView page, String url) {
                handler.removeCallbacks(timeout);
                handler.postDelayed(() -> page.evaluateJavascript("document.body.dataset.state", state -> {
                    log(who + ": page finished, state " + state + ", title " + page.getTitle() + ", shown "
                            + page.isShown() + ", sized " + (page.getWidth() > 0 && page.getHeight() > 0));
                    then.run();
                }), 300);
            }
        });
        view.loadUrl(PAGE);
    }
}
