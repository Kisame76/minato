package io.github.kisame76.minato.test.webviewprompt;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.webkit.JsPromptResult;
import android.webkit.WebChromeClient;
import android.webkit.WebView;
import android.widget.FrameLayout;

import org.json.JSONArray;
import org.json.JSONException;

/**
 * A page of the app's assets (file:///android_asset/www/index.html) behind a splash calls the app through prompt(),
 * as older hybrid app frameworks do: onJsPrompt answers each call whose default starts with "gap:" with JSON
 * (Device.getInfo, Log.info, SplashScreen.hide) and confirms it, on the main thread; the page fires its ready event
 * after the first answer and asks to hide the splash. Without the hide within 10 s it logs a timeout with the splash
 * still up. It ends with "done".
 */
public class PromptActivity extends Activity {
    private static final String TAG = "webview-prompt-bridge-test";

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private View mSplash;
    private boolean mEnded;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        FrameLayout root = new FrameLayout(this);
        WebView view = new WebView(this);
        mSplash = new View(this);
        mSplash.setBackgroundColor(Color.rgb(32, 64, 160));
        root.addView(view);
        root.addView(mSplash);
        setContentView(root);
        view.getSettings().setJavaScriptEnabled(true);
        view.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onJsPrompt(WebView view, String url, String message, String defaultValue,
                    JsPromptResult result) {
                if (defaultValue == null || !defaultValue.startsWith("gap:")) {
                    return false;
                }
                result.confirm(call(message, Looper.myLooper() == Looper.getMainLooper()));
                return true;
            }
        });
        mHandler.postDelayed(() -> end("timeout: the splash is still up"), 10000);
        view.loadUrl("file:///android_asset/www/index.html");
    }

    /** One call of the page: its answer as JSON. */
    private String call(String message, boolean onMain) {
        try {
            JSONArray call = new JSONArray(message);
            String name = call.getString(0) + "." + call.getString(1);
            log("call " + name + (onMain ? "" : " off the main thread"));
            switch (name) {
                case "Device.getInfo":
                    return "{\"platform\":\"Android\"}";
                case "Log.info":
                    log("page: " + call.getJSONArray(2).getString(0));
                    return "null";
                case "SplashScreen.hide":
                    mSplash.setVisibility(View.GONE);
                    mHandler.postDelayed(() -> end("splash hidden"), 500);
                    return "null";
                default:
                    return "null";
            }
        } catch (JSONException e) {
            log("call not JSON");
            return "null";
        }
    }

    private void end(String how) {
        if (!mEnded) {
            mEnded = true;
            log(how);
            log("done");
        }
    }
}
