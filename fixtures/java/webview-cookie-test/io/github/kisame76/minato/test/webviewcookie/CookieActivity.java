package io.github.kisame76.minato.test.webviewcookie;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.CookieManager;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/**
 * ADR-0056 c3's test: a WebView's cookies and CookieManager. Before any WebView it logs what CookieManager reads (an
 * earlier start ends with an empty store, an AVD's install is fresh); it empties the store, then loads a page at the
 * base https://minato.test/ whose script sets a host-only cookie, one for the path /x, one for the domain and so for
 * its subdomains, and one that expired. Once the page has finished it logs what CookieManager reads for /, /x/y and a
 * subdomain, sets a cookie itself and logs what the page's script then reads, sorted (document.cookie's order is the
 * engine's: Chromium's and WebKit's differ, a named difference), removes all cookies and logs what is left, then
 * "done".
 * A stage that hears nothing for 10 s logs where it stalled, then "done".
 */
public class CookieActivity extends Activity {
    private static final String TAG = "webview-cookie-test";
    private static final String BASE = "https://minato.test/";
    private static final String PAGE = "<!DOCTYPE html><html><head><title>cookies</title></head><body>"
            + "<script>document.cookie = 'site=1'; document.cookie = 'deep=2; path=/x';"
            + " document.cookie = 'dom=3; domain=minato.test';"
            + " document.cookie = 'old=4; expires=Thu, 01 Jan 1970 00:00:00 GMT';</script></body></html>";
    private static final long STALL_MS = 10000;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private WebView mView;
    private String mWaiting = "the store's removal";
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
        CookieManager cookies = CookieManager.getInstance();
        log("cookies before a WebView: " + cookies.getCookie(BASE));
        mView = new WebView(this);
        setContentView(mView);
        mView.getSettings().setJavaScriptEnabled(true);
        log("accept " + cookies.acceptCookie());
        mView.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                log("page finished");
                log("cookies of /: " + cookies.getCookie(BASE));
                log("cookies of /x/y: " + cookies.getCookie(BASE + "x/y"));
                log("cookies of a subdomain: " + cookies.getCookie("https://sub.minato.test/"));
                waitFor("setCookie");
                cookies.setCookie(BASE, "app=5", set -> {
                    log("setCookie " + set);
                    waitFor("the page's read");
                    mView.evaluateJavascript("document.cookie", value -> {
                        String[] read = value.replace("\"", "").split("; ");
                        java.util.Arrays.sort(read);
                        log("the page reads, sorted: " + String.join("; ", read));
                        waitFor("removeAllCookies");
                        cookies.removeAllCookies(removed -> {
                            log("removed " + removed);
                            log("cookies of / after: " + cookies.getCookie(BASE));
                            mHandler.removeCallbacks(mStalled);
                            log("done");
                        });
                    });
                });
            }
        });
        mHandler.postDelayed(mStalled, STALL_MS);
        cookies.removeAllCookies(removed -> {
            waitFor("the page");
            mView.loadDataWithBaseURL(BASE, PAGE, "text/html", "utf-8", null);
        });
    }

    private void waitFor(String what) {
        mWaiting = what;
        mHandler.removeCallbacks(mStalled);
        mHandler.postDelayed(mStalled, STALL_MS);
    }
}
