package io.github.kisame76.minato.test.webviewlocalhost;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.util.Collections;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

/**
 * A page the app serves itself over https, as hybrid app frameworks do: a splash covers the WebView, which loads
 * https://localhost/; the app's shouldInterceptRequest answers that host from its assets (www/), and the page's script
 * fetches a second file of the same origin and then tells the app over addJavascriptInterface what it sees (origin,
 * secure context, the file), and the app hides the splash. A second round loads androidx WebViewAssetLoader's host,
 * https://appassets.androidplatform.net/assets/www/. Each round that does not tell the app within 10 s logs a
 * timeout with the splash still up. The page also tells what a missing file of the app's host answers (the app's
 * 404), whether the cookie of the app's answer for the page (Set-Cookie) is the page's, and how a fetch of a host the
 * app does not serve ends (the network: a name that never resolves). It ends with the paths the app served, the
 * errors the client heard, then "done".
 */
public class LocalhostActivity extends Activity {
    private static final String TAG = "webview-localhost-test";
    private static final String[] PAGES = {
        "https://localhost/", "https://appassets.androidplatform.net/assets/www/index.html",
    };

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final Set<String> mServed = new TreeSet<>();
    private final Set<String> mErrors = new TreeSet<>();
    private WebView mView;
    private View mSplash;
    private int mRound = -1;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    /** What the page calls, on the bridge's thread. */
    public final class Bridge {
        @JavascriptInterface
        public void postMessage(String json) {
            mHandler.post(() -> told(json));
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        FrameLayout root = new FrameLayout(this);
        mView = new WebView(this);
        mSplash = new View(this);
        mSplash.setBackgroundColor(Color.rgb(32, 64, 160));
        root.addView(mView);
        root.addView(mSplash);
        setContentView(root);
        mView.getSettings().setJavaScriptEnabled(true);
        mView.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                return serve(request);
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                // Without the code: a name that never resolves gives -2 or -1, as the network around it answers.
                mErrors.add((request.isForMainFrame() ? "main frame" : "subresource") + " of "
                        + request.getUrl().getHost());
            }
        });
        mView.addJavascriptInterface(new Bridge(), "nativeBridge");
        next();
    }

    /**
     * The app's own hosts from its assets, as the frameworks' local servers answer them, a missing file with 404 (so
     * that Chromium's favicon request fails quietly); null for any other host.
     */
    private WebResourceResponse serve(WebResourceRequest request) {
        String host = request.getUrl().getHost();
        String path = request.getUrl().getPath();
        if (path == null) {
            return null;
        }
        String asset;
        if ("localhost".equals(host)) {
            asset = "www" + (path.equals("/") ? "/index.html" : path);
        } else if ("appassets.androidplatform.net".equals(host)) {
            asset = path.startsWith("/assets/") ? path.substring("/assets/".length()) : "missing" + path;
        } else {
            return null;
        }
        String mime = asset.endsWith(".html") ? "text/html" : asset.endsWith(".js") ? "application/javascript"
                : asset.endsWith(".json") ? "application/json" : "application/octet-stream";
        try {
            Map<String, String> headers = asset.endsWith("index.html")
                    ? Collections.singletonMap("Set-Cookie", "served=yes; Path=/") : Collections.emptyMap();
            WebResourceResponse response = new WebResourceResponse(mime, "UTF-8", 200, "OK", headers,
                    getAssets().open(asset));
            synchronized (mServed) {
                mServed.add(host + " " + asset);
            }
            return response;
        } catch (IOException e) {
            return new WebResourceResponse("text/plain", "UTF-8", 404, "Not Found", Collections.emptyMap(),
                    new ByteArrayInputStream(new byte[0]));
        }
    }

    private void next() {
        mRound++;
        if (mRound == PAGES.length) {
            synchronized (mServed) {
                log("served " + mServed);
            }
            log("errors " + mErrors);
            log("done");
            return;
        }
        mSplash.setVisibility(View.VISIBLE);
        final int round = mRound;
        mHandler.postDelayed(() -> {
            if (mRound == round) {
                log("round " + round + " timeout: the splash is still up");
                next();
            }
        }, 10000);
        mView.loadUrl(PAGES[round]);
    }

    private void told(String json) {
        try {
            JSONObject message = new JSONObject(json);
            log("round " + mRound + " page: origin " + message.optString("origin") + ", secure "
                    + message.optBoolean("secure") + ", title " + message.optString("title") + ", data "
                    + message.optString("data", "-") + ", missing " + message.optString("missing", "-") + ", cookie "
                    + message.optString("cookie", "-") + ", remote " + message.optString("remote", "-")
                    + (message.has("error") ? ", error " + message.optString("error") : ""));
        } catch (JSONException e) {
            log("round " + mRound + " page: not JSON");
        }
        mSplash.setVisibility(View.GONE);
        log("round " + mRound + " splash hidden");
        mHandler.postDelayed(this::next, 500);
    }
}
