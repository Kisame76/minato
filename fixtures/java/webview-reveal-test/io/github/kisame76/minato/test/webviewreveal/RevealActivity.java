package io.github.kisame76.minato.test.webviewreveal;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.PixelCopy;
import android.view.View;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import java.io.IOException;

/**
 * A WebView shown the ways apps show one once its page has loaded, over a window background of its own color, so that
 * the window's pixels tell what shows at the view's center. The page (green, #22aa66) comes from the app's assets at
 * https://appassets.androidplatform.net/assets/www/, answered in shouldInterceptRequest. Rounds, each with a new
 * WebView: plain) shown from the start; fade) alpha 0, faded in to 1 over 300 ms at onPageFinished; late) INVISIBLE,
 * made VISIBLE at onPageFinished; layer) inside a parent drawn through a hardware layer (setLayerType). One second
 * after the reveal, PixelCopy copies the window's 4 x 4 pixels at the WebView's center: "page" (the page's green),
 * "transparent" (alpha 0), or the color. Each round logs that, or a timeout without onPageFinished after 10 s. It ends
 * with "done".
 */
public class RevealActivity extends Activity {
    private static final String TAG = "webview-reveal-test";
    private static final String PAGE = "https://appassets.androidplatform.net/assets/www/index.html";
    private static final String[] ROUNDS = {"plain", "fade", "late", "layer"};

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private int mRound = -1;
    private boolean mEnded;
    private WebView mView;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().getDecorView().setBackgroundColor(Color.rgb(160, 32, 64));
        next();
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
        mEnded = false;
        final String round = ROUNDS[mRound];
        FrameLayout root = new FrameLayout(this);
        FrameLayout parent = new FrameLayout(this);
        if (round.equals("layer")) {
            parent.setLayerType(View.LAYER_TYPE_HARDWARE, null);
        }
        mView = new WebView(this);
        if (round.equals("fade")) {
            mView.setAlpha(0f);
        } else if (round.equals("late")) {
            mView.setVisibility(View.INVISIBLE);
        }
        parent.addView(mView);
        root.addView(parent);
        setContentView(root);
        mView.getSettings().setJavaScriptEnabled(true);
        mView.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                if (view == mView) {
                    reveal(round);
                }
            }

            @Override
            public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                String path = request.getUrl().getPath();
                if (!"appassets.androidplatform.net".equals(request.getUrl().getHost()) || path == null
                        || !path.startsWith("/assets/")) {
                    return null;
                }
                try {
                    return new WebResourceResponse("text/html", "UTF-8",
                            getAssets().open(path.substring("/assets/".length())));
                } catch (IOException e) {
                    return null;
                }
            }
        });
        final int index = mRound;
        mHandler.postDelayed(() -> {
            if (mRound == index && !mEnded) {
                end(round + ": no onPageFinished after 10 s");
            }
        }, 10000);
        mView.loadUrl(PAGE);
    }

    private void reveal(String round) {
        if (round.equals("fade")) {
            mView.animate().alpha(1f).setDuration(300).start();
        } else if (round.equals("late")) {
            mView.setVisibility(View.VISIBLE);
        }
        final int index = mRound;
        mHandler.postDelayed(() -> {
            if (mRound == index) {
                copy(round);
            }
        }, 1000);
    }

    /** The window's pixels at the WebView's center. */
    private void copy(String round) {
        int[] at = new int[2];
        mView.getLocationInWindow(at);
        int x = at[0] + mView.getWidth() / 2;
        int y = at[1] + mView.getHeight() / 2;
        final Bitmap pixels = Bitmap.createBitmap(4, 4, Bitmap.Config.ARGB_8888);
        PixelCopy.request(getWindow(), new Rect(x - 2, y - 2, x + 2, y + 2), pixels, result -> {
            if (result != PixelCopy.SUCCESS) {
                end(round + ": PixelCopy " + result);
                return;
            }
            int color = pixels.getPixel(2, 2);
            String seen = Color.alpha(color) == 0 ? "transparent"
                    : near(color, Color.rgb(0x22, 0xaa, 0x66)) ? "page"
                    : near(color, Color.rgb(160, 32, 64)) ? "the window's background"
                    : String.format("#%08x", color);
            end(round + ": the window at the WebView's center shows " + seen);
        }, mHandler);
    }

    private static boolean near(int a, int b) {
        return Math.abs(Color.red(a) - Color.red(b)) <= 8 && Math.abs(Color.green(a) - Color.green(b)) <= 8
                && Math.abs(Color.blue(a) - Color.blue(b)) <= 8 && Color.alpha(a) == 255;
    }

    private void end(String line) {
        if (mEnded) {
            return;
        }
        mEnded = true;
        log(line);
        mHandler.post(this::next);
    }
}
