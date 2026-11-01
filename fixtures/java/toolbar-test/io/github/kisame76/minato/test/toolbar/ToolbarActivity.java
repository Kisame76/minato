package io.github.kisame76.minato.test.toolbar;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toolbar;

/**
 * The platform Toolbar (the platform action bar's step 2): a Toolbar in the layout of a NoActionBar activity, as apps
 * use one and as the action bar's own implementation uses its own. It logs what Android 15 gives after the layout: the
 * toolbar's children by kind (the navigation button, the title and subtitle texts, the action menu view and its item
 * views), the title, the subtitle, the menu, whether a size is laid out, the clicks on the navigation button and on an
 * action item, the overflow's showing and hiding, and what the inflated menu adds. "done" ends the run.
 */
public class ToolbarActivity extends Activity {
    private static String TAG = "toolbar-test";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private Toolbar mToolbar;
    private boolean mStarted;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TAG = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        mToolbar = new Toolbar(this);
        mToolbar.setTitle("Title");
        mToolbar.setSubtitle("Subtitle");
        mToolbar.setNavigationIcon(new ColorDrawable(Color.RED));
        mToolbar.setNavigationContentDescription("Up");
        mToolbar.setNavigationOnClickListener(v -> log("navigation clicked"));
        mToolbar.setOnMenuItemClickListener(item -> {
            log("menu item clicked " + item.getTitle());
            return true;
        });
        final Menu menu = mToolbar.getMenu();
        menu.add(Menu.NONE, 1, 1, "Action").setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        menu.add(Menu.NONE, 2, 2, "Overflow").setShowAsAction(MenuItem.SHOW_AS_ACTION_NEVER);
        root.addView(mToolbar, new LinearLayout.LayoutParams(-1, -2));
        final TextView body = new TextView(this);
        body.setText("body");
        root.addView(body);
        setContentView(root);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            mHandler.postDelayed(() -> step(0), 800);
        }
    }

    private void dump(String what, View view, String indent) {
        final StringBuilder line = new StringBuilder(indent + view.getClass().getSimpleName());
        if (view instanceof TextView) {
            line.append(" \"").append(((TextView) view).getText()).append('"');
        }
        if (view.getContentDescription() != null) {
            line.append(" description ").append(view.getContentDescription());
        }
        line.append(view.getVisibility() == View.VISIBLE ? "" : " not visible");
        log(what + line);
        if (view instanceof ViewGroup && !(view instanceof Toolbar && indent.length() > 0)) {
            final ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                dump(what, group.getChildAt(i), indent + "  ");
            }
        }
    }

    private void step(int number) {
        switch (number) {
            case 0:
                log("laid out " + (mToolbar.getWidth() > 0 && mToolbar.getHeight() > 0) + ", title "
                        + mToolbar.getTitle() + ", subtitle " + mToolbar.getSubtitle() + ", menu items "
                        + mToolbar.getMenu().size() + ", navigation " + mToolbar.getNavigationContentDescription());
                dump("tree: ", mToolbar, "");
                break;
            case 1:
                for (int i = 0; i < mToolbar.getChildCount(); i++) {
                    if (mToolbar.getChildAt(i) instanceof ImageButton) {
                        mToolbar.getChildAt(i).performClick();
                    }
                }
                break;
            case 2:
                log("performIdentifierAction action: " + mToolbar.getMenu().performIdentifierAction(1, 0));
                break;
            case 3:
                log("showOverflowMenu " + mToolbar.showOverflowMenu() + ", showing " + mToolbar.isOverflowMenuShowing());
                break;
            case 4:
                log("showing before hide " + mToolbar.isOverflowMenuShowing() + ", hideOverflowMenu "
                        + mToolbar.hideOverflowMenu() + ", showing " + mToolbar.isOverflowMenuShowing());
                break;
            case 5:
                mToolbar.inflateMenu(getResources().getIdentifier("toolbar", "menu", getPackageName()));
                log("inflated, menu items " + mToolbar.getMenu().size());
                break;
            case 6:
                mToolbar.setTitle("Other");
                mToolbar.setSubtitle((CharSequence) null);
                mToolbar.setNavigationIcon(null);
                log("changed, title " + mToolbar.getTitle() + ", subtitle " + mToolbar.getSubtitle());
                dump("tree after: ", mToolbar, "");
                break;
            default:
                log("done");
                return;
        }
        mHandler.postDelayed(() -> step(number + 1), 800);
    }
}
