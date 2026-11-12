package io.github.kisame76.minato.test.webviewcommit;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * A splash over a WebView, taken away where apps take it away. The page comes from the app's own assets at
 * https://appassets.androidplatform.net/assets/www/ (androidx WebViewAssetLoader's host), answered in
 * shouldInterceptRequest, with 50 subresources (2 style sheets, 46 images, 2 scripts); the view has a WebViewClient,
 * no WebChromeClient and no JavaScript interface. Six rounds, each with a new WebView and splash:
 * a) the splash goes in onPageFinished; b) in onPageCommitVisible; c) in the VisualStateCallback that
 * onPageCommitVisible posts; d-a, d-b, d-c) as a SplashScreen keep-on-screen condition: a pre-draw listener of the
 * content view cancels each draw while the condition holds, and that point clears it; the splash goes at the next
 * pre-draw. Each round logs WebViewClient's callbacks of the main frame (see order()), how often the others came
 * (shouldInterceptRequest, onLoadResource, onScaleChanged, errors), and where the splash went, or that it was still up
 * after 10 s. The page names an empty icon, so that Chromium asks for no /favicon.ico. It ends with "done".
 */
public class CommitActivity extends Activity {
    private static final String TAG = "webview-commit-test";
    private static final String PAGE = "https://appassets.androidplatform.net/assets/www/index.html";
    private static final String[] ROUNDS = {"a", "b", "c", "d-a", "d-b", "d-c"};

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final AtomicInteger mIntercepted = new AtomicInteger();
    private final AtomicInteger mServed = new AtomicInteger();
    private final List<String> mOrder = new ArrayList<>();
    private int mLoadResources;
    private int mScaleChanges;
    private int mErrors;
    private int mRound = -1;
    private String mRemovedAt;
    private boolean mFinished;
    private boolean mKeep;
    private boolean mEnded;
    private WebView mView;
    private View mSplash;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        next();
    }

    /** The point the round takes the splash away at, or clears its keep-on-screen condition at. */
    private String point() {
        String round = ROUNDS[mRound];
        return round.substring(round.length() - 1);
    }

    private boolean keeps() {
        return ROUNDS[mRound].startsWith("d-");
    }

    private void next() {
        if (mView != null) {
            mView.destroy();
        }
        mRound++;
        if (mRound == ROUNDS.length) {
            log("done");
            return;
        }
        mOrder.clear();
        mIntercepted.set(0);
        mServed.set(0);
        mLoadResources = 0;
        mScaleChanges = 0;
        mErrors = 0;
        mRemovedAt = null;
        mFinished = false;
        mEnded = false;
        final FrameLayout root = new FrameLayout(this);
        mView = new WebView(this);
        mSplash = new View(this);
        mSplash.setBackgroundColor(Color.rgb(32, 64, 160));
        root.addView(mView);
        root.addView(mSplash);
        setContentView(root);
        mView.getSettings().setJavaScriptEnabled(true);
        mView.setWebViewClient(new Client());
        mKeep = keeps();
        if (mKeep) {
            // androidx's SplashScreen on Android 12 and later: the content view's pre-draw listener cancels each draw
            // while the condition holds, then removes itself and lets the splash go.
            root.getViewTreeObserver().addOnPreDrawListener(new ViewTreeObserver.OnPreDrawListener() {
                @Override
                public boolean onPreDraw() {
                    if (mKeep) {
                        return false;
                    }
                    root.getViewTreeObserver().removeOnPreDrawListener(this);
                    remove("a pre-draw after the condition cleared");
                    return true;
                }
            });
        }
        final int round = mRound;
        mHandler.postDelayed(() -> {
            if (mRound == round) {
                end("the splash is still up after 10 s");
            }
        }, 10000);
        mView.loadUrl(PAGE);
    }

    /** The round's point came: take the splash away, or clear the condition. */
    private void reached(String point) {
        if (!point.equals(point())) {
            return;
        }
        if (keeps()) {
            mKeep = false;
            mView.invalidate();
        } else {
            remove(point.equals("a") ? "onPageFinished" : point.equals("b") ? "onPageCommitVisible"
                    : "the VisualStateCallback");
        }
    }

    private void remove(String where) {
        if (mRemovedAt != null) {
            return;
        }
        mSplash.setVisibility(View.GONE);
        mRemovedAt = where;
        maybeEnd();
    }

    private void maybeEnd() {
        if (mRemovedAt != null && mFinished) {
            final int round = mRound;
            mHandler.postDelayed(() -> {
                if (mRound == round) {
                    end("the splash went at " + mRemovedAt);
                }
            }, 1000);
        }
    }

    private void end(String outcome) {
        if (mEnded) {
            return;
        }
        mEnded = true;
        log("round " + ROUNDS[mRound] + ": " + order());
        log("round " + ROUNDS[mRound] + ": intercepted " + mIntercepted.get() + ", served " + mServed.get()
                + ", onLoadResource " + mLoadResources + ", onScaleChanged " + (mScaleChanges > 0 ? "yes" : "no")
                + ", errors " + mErrors);
        log("round " + ROUNDS[mRound] + ": " + outcome);
        mHandler.post(this::next);
    }

    /**
     * The callbacks in their order up to the commit; after it, onPageFinished, onPageCommitVisible and the
     * VisualStateCallback come in either order on Android (the page's last subresource or its first frame first), so
     * those are listed sorted, with whether the VisualStateCallback came after onPageCommitVisible.
     */
    private String order() {
        List<String> first = new ArrayList<>();
        List<String> rest = new ArrayList<>();
        for (String name : mOrder) {
            boolean late = name.equals("onPageFinished") || name.equals("onPageCommitVisible")
                    || name.equals("VisualStateCallback");
            (late || !rest.isEmpty() ? rest : first).add(name);
        }
        List<String> sorted = new ArrayList<>(rest);
        Collections.sort(sorted);
        int visible = mOrder.indexOf("onPageCommitVisible");
        int state = mOrder.indexOf("VisualStateCallback");
        return String.join(", ", first) + "; then " + String.join(", ", sorted)
                + (visible >= 0 && state >= 0 ? "; the VisualStateCallback after onPageCommitVisible "
                + (state > visible ? "yes" : "no") : "");
    }

    private final class Client extends WebViewClient {
        @Override
        public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
            mOrder.add("shouldOverrideUrlLoading");
            return false;
        }

        @Override
        public void onPageStarted(WebView view, String url, Bitmap favicon) {
            mOrder.add("onPageStarted");
        }

        @Override
        public void doUpdateVisitedHistory(WebView view, String url, boolean isReload) {
            mOrder.add("doUpdateVisitedHistory");
        }

        @Override
        public void onPageCommitVisible(WebView view, String url) {
            if (view != mView) {
                return;
            }
            mOrder.add("onPageCommitVisible");
            reached("b");
            final int round = mRound;
            view.postVisualStateCallback(1, new WebView.VisualStateCallback() {
                @Override
                public void onComplete(long requestId) {
                    if (mRound == round) {
                        mOrder.add("VisualStateCallback");
                        reached("c");
                    }
                }
            });
        }

        @Override
        public void onPageFinished(WebView view, String url) {
            if (view != mView) {
                return;
            }
            mOrder.add("onPageFinished");
            mFinished = true;
            reached("a");
            maybeEnd();
        }

        @Override
        public void onLoadResource(WebView view, String url) {
            mLoadResources++;
        }

        @Override
        public void onScaleChanged(WebView view, float oldScale, float newScale) {
            mScaleChanges++;
        }

        @Override
        public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
            mErrors++;
        }

        @Override
        public void onReceivedHttpError(WebView view, WebResourceRequest request, WebResourceResponse response) {
            mErrors++;
        }

        @Override
        public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
            mIntercepted.incrementAndGet();
            String path = request.getUrl().getPath();
            if (!"appassets.androidplatform.net".equals(request.getUrl().getHost()) || path == null
                    || !path.startsWith("/assets/")) {
                return null;
            }
            String asset = path.substring("/assets/".length());
            String mime = asset.endsWith(".html") ? "text/html" : asset.endsWith(".js") ? "application/javascript"
                    : asset.endsWith(".css") ? "text/css" : asset.endsWith(".png") ? "image/png"
                    : "application/octet-stream";
            try {
                WebResourceResponse response = new WebResourceResponse(mime, "UTF-8", getAssets().open(asset));
                mServed.incrementAndGet();
                return response;
            } catch (IOException e) {
                return new WebResourceResponse("text/plain", "UTF-8", 404, "Not Found", Collections.emptyMap(),
                        new ByteArrayInputStream(new byte[0]));
            }
        }
    }
}
