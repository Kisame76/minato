package io.github.kisame76.minato.test.widgets2;

import android.annotation.TargetApi;
import android.app.Activity;
import android.app.PendingIntent;
import android.content.Intent;
import android.os.Parcel;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.net.http.HttpResponseCache;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.EmojiConsistency;
import android.text.method.HideReturnsTransformationMethod;
import android.util.EventLogTags;
import android.util.Log;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.inspector.WindowInspector;
import android.widget.ActionMenuView;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.MediaController;
import android.widget.PopupMenu;
import android.widget.ProgressBar;
import android.widget.RemoteViews;
import android.widget.TextView;
import android.widget.ZoomButtonsController;
import java.io.File;
import java.util.ArrayList;

/**
 * The test of framework breadth batch F4b: PopupMenu, ActionMenuView, MediaController, ZoomButtonsController,
 * HideReturnsTransformationMethod, HttpResponseCache, WindowInspector, EmojiConsistency, EventLogTags and TargetApi
 * as an app uses them, in the window of the launcher activity. It runs on the main thread when the window first has
 * the focus, part after part with a pause, and logs under the tag f4b-widgets-test; tools/android-reference.sh takes
 * the log from Android 15, and tools/f4b-widgets-check.sh compares Minato's with it. Popup windows are not shown on
 * Minato yet: a menu is built and configured, and its show is called without a check of what the window does.
 */
public class WidgetsActivity extends Activity {
    static final String TAG = "f4b-widgets-test";
    private static final int SETTLE_MS = 700;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private FrameLayout mRoot;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Part {
        void run() throws Exception;
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            StringBuilder line = new StringBuilder(name);
            for (Throwable c = t; c != null; c = c.getCause()) {
                line.append(": ").append(c.getClass().getName()).append(": ").append(c.getMessage());
            }
            log(line.toString());
        }
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        mRoot = new FrameLayout(this);
        setContentView(mRoot);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        final Runnable[] steps = {
            () -> part("popupMenu", this::popupMenu),
            () -> part("actionMenuView", this::actionMenuView),
            () -> part("mediaController", this::mediaController),
            () -> part("zoomButtons", this::zoomButtons),
            () -> part("windowMetrics", this::windowMetrics),
            () -> part("remoteViews", this::remoteViews),
            () -> part("hideReturns", this::hideReturns),
            () -> part("httpResponseCache", this::httpResponseCache),
            () -> part("windowInspector", this::windowInspector),
            () -> part("smallClasses", this::smallClasses),
            () -> {
                log("done");
                finish();
            },
        };
        final int[] index = {0};
        Runnable next = new Runnable() {
            @Override
            public void run() {
                steps[index[0]++].run();
                if (index[0] < steps.length) {
                    mHandler.postDelayed(this, SETTLE_MS);
                }
            }
        };
        mHandler.post(next);
    }

    /** Lays the view out at a fixed width, draws it and says whether anything was drawn. */
    private String shown(View view) {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.addView(view, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        root.measure(View.MeasureSpec.makeMeasureSpec(1080, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(2400, View.MeasureSpec.AT_MOST));
        root.layout(0, 0, root.getMeasuredWidth(), root.getMeasuredHeight());
        Bitmap bitmap = Bitmap.createBitmap(Math.max(1, root.getWidth()), Math.max(1, root.getHeight()),
                Bitmap.Config.ARGB_8888);
        root.draw(new Canvas(bitmap));
        int first = bitmap.getPixel(0, 0);
        boolean varied = false;
        for (int y = 0; y < bitmap.getHeight() && !varied; y += 7) {
            for (int x = 0; x < bitmap.getWidth(); x += 7) {
                if (bitmap.getPixel(x, y) != first) {
                    varied = true;
                    break;
                }
            }
        }
        return "measured=" + (view.getMeasuredHeight() > 0) + " laidOut=" + (view.getHeight() > 0) + " drawn=" + varied;
    }

    private void popupMenu() {
        TextView anchor = new TextView(this);
        anchor.setText("anchor");
        mRoot.addView(anchor);
        PopupMenu popup = new PopupMenu(this, anchor);
        Menu menu = popup.getMenu();
        MenuItem one = menu.add(0, 1, 0, "One");
        menu.add(0, 2, 1, "Two").setCheckable(true).setChecked(true);
        menu.addSubMenu(0, 3, 2, "More").add(0, 4, 0, "Three");
        final ArrayList<String> heard = new ArrayList<>();
        popup.setOnMenuItemClickListener(item -> {
            heard.add(item.getItemId() + ":" + item.getTitle());
            return true;
        });
        popup.setOnDismissListener(p -> heard.add("dismissed"));
        popup.setGravity(android.view.Gravity.END);
        log("popupMenu: size=" + menu.size() + " first=" + one.getTitle() + " checked=" + menu.findItem(2).isChecked()
                + " hasSub=" + menu.findItem(3).hasSubMenu() + " gravity=" + popup.getGravity());
        menu.performIdentifierAction(1, 0);
        popup.show();
        popup.dismiss();
        log("popupMenu heard: " + heard);
    }

    private void actionMenuView() {
        ActionMenuView view = new ActionMenuView(this);
        Menu menu = view.getMenu();
        menu.add(0, 1, 0, "Search").setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        menu.add(0, 2, 1, "Share").setShowAsAction(MenuItem.SHOW_AS_ACTION_NEVER);
        log("actionMenuView: size=" + menu.size() + " overflowReserved=" + view.isOverflowReserved()
                + " children=" + view.getChildCount() + " popupTheme=" + view.getPopupTheme());
        final ArrayList<String> heard = new ArrayList<>();
        view.setOnMenuItemClickListener(item -> {
            heard.add(item.getTitle().toString());
            return true;
        });
        view.setPopupTheme(android.R.style.Theme_Material);
        log("actionMenuView " + shown(view) + " popupTheme=" + (view.getPopupTheme() == android.R.style.Theme_Material)
                + " children=" + view.getChildCount());
    }

    private void mediaController() {
        MediaController controller = new MediaController(this, false);
        controller.setMediaPlayer(new MediaController.MediaPlayerControl() {
            @Override
            public void start() {
            }

            @Override
            public void pause() {
            }

            @Override
            public int getDuration() {
                return 60000;
            }

            @Override
            public int getCurrentPosition() {
                return 1000;
            }

            @Override
            public void seekTo(int pos) {
            }

            @Override
            public boolean isPlaying() {
                return false;
            }

            @Override
            public int getBufferPercentage() {
                return 50;
            }

            @Override
            public boolean canPause() {
                return true;
            }

            @Override
            public boolean canSeekBackward() {
                return true;
            }

            @Override
            public boolean canSeekForward() {
                return true;
            }

            @Override
            public int getAudioSessionId() {
                return 0;
            }
        });
        controller.setAnchorView(mRoot);
        controller.setEnabled(true);
        log("mediaController: showing=" + controller.isShowing() + " enabled=" + controller.isEnabled());
        controller.hide();
        log("mediaController hidden: showing=" + controller.isShowing());
    }

    private void zoomButtons() {
        ZoomButtonsController zoom = new ZoomButtonsController(mRoot);
        final ArrayList<String> heard = new ArrayList<>();
        zoom.setOnZoomListener(new ZoomButtonsController.OnZoomListener() {
            @Override
            public void onVisibilityChanged(boolean visible) {
                heard.add("visible " + visible);
            }

            @Override
            public void onZoom(boolean zoomIn) {
                heard.add("zoom " + zoomIn);
            }
        });
        zoom.setZoomInEnabled(true);
        zoom.setZoomOutEnabled(false);
        zoom.setAutoDismissed(false);
        log("zoomButtons: visible=" + zoom.isVisible() + " autoDismissed=" + zoom.isAutoDismissed()
                + " zoomControls=" + (zoom.getZoomControls() != null) + " container=" + (zoom.getContainer() != null));
    }

    private void windowMetrics() {
        android.view.WindowManager manager = getWindowManager();
        android.view.WindowMetrics current = manager.getCurrentWindowMetrics();
        android.view.WindowMetrics max = manager.getMaximumWindowMetrics();
        log("windowMetrics: currentSize=" + (current.getBounds().width() > 0 && current.getBounds().height() > 0)
                + " maxCovers=" + (max.getBounds().width() >= current.getBounds().width()
                && max.getBounds().height() >= current.getBounds().height()) + " insets="
                + (current.getWindowInsets() != null) + " density="
                + (current.getDensity() == getResources().getDisplayMetrics().density));
    }

    /** apply and reapply of RemoteViews, as a notification, a widget or an app's own custom view uses them. */
    private void remoteViews() {
        int layout = getResources().getIdentifier("remote", "layout", getPackageName());
        int text = getResources().getIdentifier("text", "id", getPackageName());
        int progress = getResources().getIdentifier("progress", "id", getPackageName());
        int image = getResources().getIdentifier("image", "id", getPackageName());
        int button = getResources().getIdentifier("button", "id", getPackageName());
        RemoteViews views = new RemoteViews(getPackageName(), layout);
        views.setTextViewText(text, "hello");
        views.setProgressBar(progress, 100, 40, false);
        views.setContentDescription(image, "picture");
        views.setViewVisibility(button, View.GONE);
        views.setOnClickPendingIntent(button, PendingIntent.getActivity(this, 0,
                new Intent(this, WidgetsActivity.class), PendingIntent.FLAG_IMMUTABLE));
        View applied = views.apply(this, new FrameLayout(this));
        log("remoteViews apply: text=" + ((TextView) applied.findViewById(text)).getText() + " progress="
                + ((ProgressBar) applied.findViewById(progress)).getProgress() + " description="
                + applied.findViewById(image).getContentDescription() + " button="
                + (applied.findViewById(button).getVisibility() == View.GONE ? "gone" : "visible")
                + " layout=" + (layout == views.getLayoutId() ? "same" : "other"));
        views.setTextViewText(text, "second");
        views.reapply(this, applied);
        log("remoteViews reapply: text=" + ((TextView) applied.findViewById(text)).getText());
        Parcel parcel = Parcel.obtain();
        views.writeToParcel(parcel, 0);
        parcel.setDataPosition(0);
        RemoteViews copy = RemoteViews.CREATOR.createFromParcel(parcel);
        parcel.recycle();
        View again = copy.apply(this, new FrameLayout(this));
        log("remoteViews parcel: text=" + ((TextView) again.findViewById(text)).getText() + " package="
                + (copy.getPackage().equals(getPackageName())));
        RemoteViews clone = new RemoteViews(views);
        log("remoteViews clone: text=" + ((TextView) clone.apply(this, new FrameLayout(this))
                .findViewById(text)).getText());
    }

    private void hideReturns() {
        TextView view = new TextView(this);
        CharSequence out = HideReturnsTransformationMethod.getInstance().getTransformation("a\nb\rc", view);
        StringBuilder codes = new StringBuilder();
        for (int i = 0; i < out.length(); i++) {
            codes.append(Integer.toHexString(out.charAt(i))).append(' ');
        }
        log("hideReturns: " + codes.toString().trim() + " same=" + (HideReturnsTransformationMethod.getInstance()
                == HideReturnsTransformationMethod.getInstance()));
    }

    private void httpResponseCache() throws Exception {
        File dir = new File(getCacheDir(), "http");
        HttpResponseCache cache = HttpResponseCache.install(dir, 1 << 20);
        log("httpResponseCache: installed=" + (cache != null) + " same=" + (HttpResponseCache.getInstalled() == cache)
                + " hits=" + cache.getHitCount() + " requests=" + cache.getRequestCount() + " network="
                + cache.getNetworkCount() + " max=" + cache.maxSize());
        cache.delete();
        log("httpResponseCache deleted: installed=" + (HttpResponseCache.getInstalled() != null));
    }

    private void windowInspector() {
        log("windowInspector: windows=" + (WindowInspector.getGlobalWindowViews().size() > 0));
    }

    @TargetApi(26)
    private void smallClasses() throws Exception {
        log("emoji: " + (EmojiConsistency.getEmojiConsistencySet() != null));
        EventLogTags tags = new EventLogTags();
        log("eventLogTags: byName=" + (tags.get("no_such_tag") == null) + " byTag=" + (tags.get(-1) == null));
    }
}
