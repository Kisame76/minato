package io.github.kisame76.minato.test.sdkextensions;

import android.app.Activity;
import android.os.Bundle;
import android.os.ext.SdkExtensions;
import android.util.Log;
import android.widget.TextView;
import java.util.Map;
import java.util.TreeMap;

/**
 * The activity of sdk-extensions-test: asks SdkExtensions for the version of each extension an app can name, for all
 * of them, and for two numbers that are none. It says "ready" once its window has the focus and "done" at the end.
 */
public class ExtensionsActivity extends Activity {
    private static final String TAG = "sdk-extensions-test";
    private boolean ready;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText(TAG);
        setContentView(text);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || ready) {
            return;
        }
        ready = true;
        Log.i(TAG, "ready");
        int[] extensions = {30, 31, 33, 34, 35, SdkExtensions.AD_SERVICES, 29, 0, -1, 32, 36, 12345};
        for (int extension : extensions) {
            try {
                Log.i(TAG, "getExtensionVersion " + extension + " = " + SdkExtensions.getExtensionVersion(extension));
            } catch (RuntimeException e) {
                Log.i(TAG, "getExtensionVersion " + extension + " throws " + e);
            }
        }
        Map<Integer, Integer> all = SdkExtensions.getAllExtensionVersions();
        Log.i(TAG, "getAllExtensionVersions " + new TreeMap<>(all));
        try {
            all.put(1, 1);
            Log.i(TAG, "the map can be changed");
        } catch (RuntimeException e) {
            Log.i(TAG, "the map cannot be changed: " + e);
        }
        Log.i(TAG, "done");
    }
}
