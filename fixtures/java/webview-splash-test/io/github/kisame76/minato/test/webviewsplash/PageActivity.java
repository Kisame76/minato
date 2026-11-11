package io.github.kisame76.minato.test.webviewsplash;

import android.app.Activity;
import android.os.Bundle;
import android.webkit.WebView;

/** The second activity: a WebView as its content from the start, which loads the asset page; then "done". */
public class PageActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        WebView view = new WebView(this);
        setContentView(view);
        Pages.load("second activity", view, () -> Pages.log("done"));
    }
}
