package io.github.kisame76.minato.test.activitystart;

import android.app.Activity;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewParent;
import android.view.ViewTreeObserver;

/**
 * The launcher, behind the alias Launcher: logs its lifecycle up to its window's focus, its window configuration, where
 * its content view sits, and one measure and layout of a view with its global layout and a View.post.
 */
public class StartActivity extends Activity {
    private static final String TAG = StartApplication.TAG;
    private boolean mFocused;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "onCreate, saved state " + (savedInstanceState != null));
        Intent intent = getIntent();
        Log.i(TAG, "component " + getComponentName().getShortClassName() + ", the intent's "
                + intent.getComponent().getShortClassName() + ", action " + intent.getAction() + ", categories "
                + intent.getCategories());
        Log.i(TAG, "window configuration " + windowConfiguration(getResources().getConfiguration()));
        // As view-tree-test, by name: tools/build-java-test-app.sh writes no R class.
        setContentView(getResources().getIdentifier("start", "layout", getPackageName()));
        View content = findViewById(android.R.id.content);
        Log.i(TAG, "content " + content.getClass().getName() + ", parents " + parents(content));
        View box = findViewById(getResources().getIdentifier("box", "id", getPackageName()));
        box.getViewTreeObserver().addOnGlobalLayoutListener(new ViewTreeObserver.OnGlobalLayoutListener() {
            @Override
            public void onGlobalLayout() {
                box.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                int[] location = new int[2];
                box.getLocationInWindow(location);
                Log.i(TAG, "global layout: content " + content.getWidth() + " x " + content.getHeight() + ", box "
                        + box.getWidth() + " x " + box.getHeight() + " at " + location[0] + " " + location[1]);
            }
        });
        box.post(() -> Log.i(TAG, "posted, attached " + box.isAttachedToWindow() + ", the root's parent "
                + box.getRootView().getParent().getClass().getName()));
    }

    @Override
    protected void onStart() {
        super.onStart();
        Log.i(TAG, "onStart");
    }

    @Override
    protected void onPostCreate(Bundle savedInstanceState) {
        super.onPostCreate(savedInstanceState);
        Log.i(TAG, "onPostCreate");
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i(TAG, "onResume");
    }

    @Override
    protected void onPostResume() {
        super.onPostResume();
        Log.i(TAG, "onPostResume");
    }

    @Override
    public void onTopResumedActivityChanged(boolean isTopResumedActivity) {
        super.onTopResumedActivityChanged(isTopResumedActivity);
        Log.i(TAG, "onTopResumedActivityChanged " + isTopResumedActivity);
    }

    @Override
    public void onAttachedToWindow() {
        super.onAttachedToWindow();
        Log.i(TAG, "onAttachedToWindow");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        Log.i(TAG, "onWindowFocusChanged " + hasFocus);
        if (hasFocus && !mFocused) {
            mFocused = true;
            Log.i(TAG, "done");
        }
    }

    /** The window configuration part of the configuration's text, which has the window's bounds and rotation. */
    private static String windowConfiguration(Configuration config) {
        String text = config.toString();
        int start = text.indexOf("winConfig={");
        int end = start < 0 ? -1 : text.indexOf('}', start);
        return end < 0 ? text : text.substring(start, end + 1);
    }

    /** The view's parents, nearest first, up to the window's root and what holds it. */
    private static String parents(View view) {
        StringBuilder out = new StringBuilder();
        for (ViewParent parent = view.getParent(); parent != null;
                parent = parent instanceof View ? parent.getParent() : null) {
            out.append(out.length() == 0 ? "" : " < ").append(parent.getClass().getName());
        }
        return out.toString();
    }
}
