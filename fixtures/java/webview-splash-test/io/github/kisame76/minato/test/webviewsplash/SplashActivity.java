package io.github.kisame76.minato.test.webviewsplash;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.webkit.WebView;

/**
 * The launcher: a splash for 300 ms, then a WebView in its place in the same window, which loads an asset page; once
 * it has finished, PageActivity, whose window has a WebView of its own, and this activity finishes.
 */
public class SplashActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        View splash = new View(this);
        splash.setBackgroundColor(Color.rgb(32, 64, 160));
        setContentView(splash);
        Pages.log("splash shown");
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            WebView view = new WebView(this);
            setContentView(view);
            Pages.log("splash replaced by the WebView");
            Pages.load("same window", view, () -> {
                startActivity(new Intent(this, PageActivity.class));
                finish();
            });
        }, 300);
    }
}
