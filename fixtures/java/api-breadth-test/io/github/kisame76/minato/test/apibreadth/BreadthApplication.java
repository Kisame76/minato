package io.github.kisame76.minato.test.apibreadth;

import android.app.Application;
import android.util.Log;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/**
 * The framework breadth's test (the compatibility plan's entry 22): each public type that assets/types.txt names, one
 * a line in the order of the batches that took it, is loaded by name, first without its initialization and then with
 * it, and logs "&lt;type&gt;: loads", or the throwable's class, after "init " when only the initialization fails.
 */
public class BreadthApplication extends Application {
    private static final String TAG = "api-breadth-test";

    @Override
    public void onCreate() {
        super.onCreate();
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(getAssets().open("types.txt"), StandardCharsets.UTF_8))) {
            for (String line; (line = reader.readLine()) != null;) {
                String name = line.trim();
                if (!name.isEmpty() && !name.startsWith("#")) {
                    Log.i(TAG, name + ": " + load(name));
                }
            }
        } catch (IOException e) {
            Log.i(TAG, "types.txt: " + e);
        }
        Log.i(TAG, "done");
    }

    private String load(String name) {
        ClassLoader loader = getClassLoader();
        try {
            Class.forName(name, false, loader);
        } catch (Throwable t) {
            return t.getClass().getName();
        }
        try {
            Class.forName(name, true, loader);
        } catch (Throwable t) {
            return "init " + t.getClass().getName();
        }
        return "loads";
    }
}
