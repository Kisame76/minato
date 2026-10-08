package io.github.kisame76.minato.test.widgets;

import android.app.Application;
import android.content.Context;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.LayerDrawable;
import android.util.Log;
import android.view.ContextThemeWrapper;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.AlphaAnimation;
import android.view.animation.Animation;
import android.view.animation.AnimationSet;
import android.view.animation.AnimationUtils;
import android.view.animation.Interpolator;
import android.view.animation.LayoutAnimationController;
import android.view.animation.RotateAnimation;
import android.view.animation.ScaleAnimation;
import android.view.animation.Transformation;
import android.view.animation.TranslateAnimation;
import android.widget.AbsListView;
import android.widget.ArrayAdapter;
import android.widget.CheckedTextView;
import android.widget.EdgeEffect;
import android.widget.ExpandableListView;
import android.widget.GridView;
import android.widget.HorizontalScrollView;
import android.widget.ImageSwitcher;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.OverScroller;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.RatingBar;
import android.widget.ScrollView;
import android.widget.Scroller;
import android.widget.SeekBar;
import android.widget.SimpleExpandableListAdapter;
import android.widget.Switch;
import android.widget.TextSwitcher;
import android.widget.TextView;
import android.widget.ToggleButton;
import android.widget.ViewFlipper;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The compatibility plan's gap 5, widgets, batch 1: AOSP's android.widget with the types it stands on, in App's order
 * of what AppCompat, Material Components and RecyclerView lean on. On Theme.Material.Light, whose colors are fixed
 * (Monet's stay out), it inflates res/layout's widgets, lays each layout out at fixed sizes in px at the M4 plan's
 * display (density 480), and logs under the tag widgets-test what they answer:
 * <ul>
 * <li>images: ImageView's matrix and its drawable's bounds at each scale type, and adjusted bounds;
 * <li>progress: a bar's range, progress and drawable levels, an indeterminate spinner as a state, SeekBar and
 * RatingBar;
 * <li>scroll: ScrollView and HorizontalScrollView with fillViewport, their ranges and scrollTo's clamping;
 * <li>scrollers: Scroller's and OverScroller's flings at three velocities, which RecyclerView's flings follow;
 * <li>edges: EdgeEffect's pull, its distance (a stretch's, since Android 12) and its release;
 * <li>compound: RadioGroup's single choice, ToggleButton's texts, Switch and CheckedTextView;
 * <li>relative, grid and table: RelativeLayout's, GridLayout's and TableLayout's placements;
 * <li>lists and expandable: ListView's, GridView's and ExpandableListView's rows;
 * <li>switchers and clocks: ViewFlipper and the text and image switchers, the clocks at texts that do not change;
 * <li>animations and interpolators: view animations at given times and interpolators at given inputs.
 * </ul>
 * No view is attached to a window, so no animation runs and every drawable's transition is ended first
 * (jumpDrawablesToCurrentState). A failing part logs "failed" and its stack under widgets-test-device, which differs by
 * device and is only shown. tools/android-reference.sh takes the log from Android 15 on the M4 plan's display, and
 * tools/widgets-check.sh compares Minato's with it.
 */
public class WidgetsApplication extends Application {
    private static final String TAG = "widgets-test";
    private static final int WIDTH = 1290;

    /** One part of the test, which may throw. */
    private interface Part {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Context themed = new ContextThemeWrapper(this, android.R.style.Theme_Material_Light);
        part("images", () -> images(themed));
        part("progress", () -> progress(themed));
        part("scroll", () -> scroll(themed));
        part("scrollers", () -> scrollers(themed));
        part("edges", () -> edges(themed));
        part("compound", () -> compound(themed));
        part("relative", () -> relative(themed));
        part("grid", () -> grid(themed));
        part("lists", () -> lists(themed));
        part("expandable", () -> expandable(themed));
        part("switchers", () -> switchers(themed));
        part("clocks", () -> clocks(themed));
        part("animations", () -> animations(themed));
        part("interpolators", () -> interpolators(themed));
        Log.i(TAG, "done");
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            Log.i(TAG, name + ": failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
    }

    /** ImageView at each scale type: its place, its matrix and its drawable's bounds. */
    private static void images(Context themed) {
        ViewGroup root = layout(themed, "images", WIDTH, 0);
        for (int i = 0; i < root.getChildCount(); i++) {
            ImageView image = (ImageView) root.getChildAt(i);
            String name = name(themed, image);
            if (name.equals("matrix")) {
                Matrix half = new Matrix();
                half.setScale(0.5f, 0.5f);
                half.postTranslate(10, 20);
                image.setImageMatrix(half);
            }
            Drawable drawable = image.getDrawable();
            Log.i(TAG, "images: " + name + " " + image.getClass().getName() + " " + place(image) + ", scale type "
                    + image.getScaleType() + ", drawable " + drawable.getIntrinsicWidth() + " x "
                    + drawable.getIntrinsicHeight() + " at " + drawable.getBounds().toShortString() + ", matrix "
                    + values(image.getImageMatrix()) + ", baseline " + image.getBaseline());
        }
    }

    /** A bar's range and levels, an indeterminate spinner as a state, a SeekBar and a RatingBar. */
    private static void progress(Context themed) {
        ViewGroup root = layout(themed, "progress", WIDTH, 0);
        for (int i = 0; i < root.getChildCount(); i++) {
            ProgressBar bar = (ProgressBar) root.getChildAt(i);
            String name = name(themed, bar);
            Log.i(TAG, "progress: " + name + " " + bar.getClass().getName() + " " + place(bar) + ", indeterminate "
                    + bar.isIndeterminate() + ", min " + bar.getMin() + ", max " + bar.getMax() + ", progress "
                    + bar.getProgress() + ", secondary " + bar.getSecondaryProgress());
            Drawable drawable = bar.isIndeterminate() ? bar.getIndeterminateDrawable() : bar.getProgressDrawable();
            StringBuilder levels = new StringBuilder();
            if (drawable instanceof LayerDrawable) {
                LayerDrawable layers = (LayerDrawable) drawable;
                for (int id : new int[] {android.R.id.background, android.R.id.secondaryProgress,
                        android.R.id.progress}) {
                    Drawable layer = layers.findDrawableByLayerId(id);
                    levels.append(layer == null ? " -" : " " + layer.getLevel());
                }
            }
            Log.i(TAG, "progress: " + name + " drawable " + drawable.getClass().getName() + ", levels" + levels);
            if (bar instanceof SeekBar) {
                SeekBar seek = (SeekBar) bar;
                Log.i(TAG, "progress: " + name + " thumb offset " + seek.getThumbOffset() + ", key increment "
                        + seek.getKeyProgressIncrement() + ", thumb at " + seek.getThumb().getBounds().toShortString());
            }
            if (bar instanceof RatingBar) {
                RatingBar rating = (RatingBar) bar;
                Log.i(TAG, "progress: " + name + " rating " + rating.getRating() + " of " + rating.getNumStars()
                        + " in steps of " + rating.getStepSize());
                rating.setRating(2.25f);
                Log.i(TAG, "progress: " + name + " set to 2.25: rating " + rating.getRating() + ", progress "
                        + rating.getProgress());
            }
        }
    }

    /** ScrollView and HorizontalScrollView: fillViewport with a short child, then a long child's range. */
    private static void scroll(Context themed) {
        ScrollView filled = new ScrollView(themed);
        filled.setFillViewport(true);
        filled.addView(new View(themed), new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 500));
        exactly(filled, WIDTH, 2000);
        Log.i(TAG, "scroll: filled, its child " + place(filled.getChildAt(0)));

        // A scroll view measures its child's height unbounded: a child of its own minimum height keeps it.
        ScrollView vertical = new ScrollView(themed);
        vertical.addView(tall(themed, 0, 5000), new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        exactly(vertical, WIDTH, 2000);
        Log.i(TAG, "scroll: vertical, its child " + place(vertical.getChildAt(0)) + ", max scroll amount "
                + vertical.getMaxScrollAmount());
        vertical.scrollTo(0, 99999);
        Log.i(TAG, "scroll: vertical to 99999: " + vertical.getScrollY());
        vertical.scrollTo(0, -50);
        Log.i(TAG, "scroll: vertical to -50: " + vertical.getScrollY());
        vertical.scrollTo(0, 1234);
        Log.i(TAG, "scroll: vertical to 1234: " + vertical.getScrollY());

        HorizontalScrollView horizontal = new HorizontalScrollView(themed);
        horizontal.setFillViewport(true);
        horizontal.addView(tall(themed, 4000, 0), new ViewGroup.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        exactly(horizontal, WIDTH, 300);
        Log.i(TAG, "scroll: horizontal, its child " + place(horizontal.getChildAt(0)) + ", max scroll amount "
                + horizontal.getMaxScrollAmount());
        horizontal.scrollTo(99999, 0);
        Log.i(TAG, "scroll: horizontal to 99999: " + horizontal.getScrollX());
    }

    /** Scroller's and OverScroller's flings at three velocities: where they end and, for Scroller, how long. */
    private static void scrollers(Context themed) {
        for (int velocity : new int[] {1000, 4000, 10000}) {
            Scroller scroller = new Scroller(themed);
            scroller.fling(0, 0, 0, velocity, 0, 0, 0, 100000);
            OverScroller over = new OverScroller(themed);
            over.fling(0, 0, 0, velocity, 0, 0, 0, 100000);
            Log.i(TAG, "scrollers: fling at " + velocity + " px/s: Scroller to " + scroller.getFinalY() + " in "
                    + scroller.getDuration() + " ms, OverScroller to " + over.getFinalY() + ", finished "
                    + over.isFinished());
        }
        Scroller scroller = new Scroller(themed);
        scroller.startScroll(0, 0, 300, 1000);
        Log.i(TAG, "scrollers: startScroll to " + scroller.getFinalX() + ", " + scroller.getFinalY() + " in "
                + scroller.getDuration() + " ms");
        OverScroller over = new OverScroller(themed);
        boolean springs = over.springBack(0, 150, 0, 0, 0, 100);
        Log.i(TAG, "scrollers: springBack from 150 into 0..100: " + springs + ", to " + over.getFinalY());
    }

    /** EdgeEffect: a pull and its distance, then release and finish. */
    private static void edges(Context themed) {
        EdgeEffect edge = new EdgeEffect(themed);
        edge.setSize(WIDTH, 2000);
        // Its type is hidden; the distance a pull leaves shows it: a stretch keeps one, a glow none.
        Log.i(TAG, "edges: finished " + edge.isFinished() + ", distance " + format(edge.getDistance()) + ", max height "
                + edge.getMaxHeight());
        float consumed = edge.onPullDistance(0.1f, 0.5f);
        Log.i(TAG, "edges: pulled 0.1: consumed " + format(consumed) + ", distance " + format(edge.getDistance())
                + ", finished " + edge.isFinished());
        float back = edge.onPullDistance(-0.04f, 0.5f);
        Log.i(TAG, "edges: pulled back 0.04: consumed " + format(back) + ", distance " + format(edge.getDistance()));
        edge.onRelease();
        Log.i(TAG, "edges: released: finished " + edge.isFinished());
        edge.finish();
        Log.i(TAG, "edges: finished " + edge.isFinished() + ", distance " + format(edge.getDistance()));
        EdgeEffect absorbed = new EdgeEffect(themed);
        absorbed.setSize(WIDTH, 2000);
        absorbed.onAbsorb(4000);
        Log.i(TAG, "edges: absorbed 4000: finished " + absorbed.isFinished());
    }

    /** RadioGroup's single choice, ToggleButton's texts, Switch's state and size, CheckedTextView. */
    private static void compound(Context themed) {
        ViewGroup root = layout(themed, "compound", WIDTH, 0);
        RadioGroup group = (RadioGroup) root.getChildAt(0);
        group.setOnCheckedChangeListener((g, id) -> Log.i(TAG, "compound: listener: " + nameOf(themed, id)));
        Log.i(TAG, "compound: group " + place(group) + ", checked " + nameOf(themed, group.getCheckedRadioButtonId()));
        group.check(id(themed, "two"));
        Log.i(TAG, "compound: checked two: " + nameOf(themed, group.getCheckedRadioButtonId()) + ", " + radios(group));
        ((RadioButton) group.getChildAt(2)).setChecked(true);
        Log.i(TAG, "compound: three set checked: " + nameOf(themed, group.getCheckedRadioButtonId()) + ", "
                + radios(group));
        group.clearCheck();
        Log.i(TAG, "compound: cleared: " + nameOf(themed, group.getCheckedRadioButtonId()) + ", " + radios(group));
        for (int i = 0; i < group.getChildCount(); i++) {
            Log.i(TAG, "compound: " + name(themed, group.getChildAt(i)) + " " + place(group.getChildAt(i)));
        }

        ToggleButton toggle = (ToggleButton) root.getChildAt(1);
        Log.i(TAG, "compound: toggle " + place(toggle) + ", on \"" + toggle.getTextOn() + "\", off \""
                + toggle.getTextOff() + "\", text \"" + toggle.getText() + "\"");
        toggle.toggle();
        Log.i(TAG, "compound: toggled: checked " + toggle.isChecked() + ", text \"" + toggle.getText() + "\"");

        Switch sw = (Switch) root.getChildAt(2);
        sw.jumpDrawablesToCurrentState();
        Log.i(TAG, "compound: switch " + place(sw) + ", checked " + sw.isChecked() + ", min width "
                + sw.getSwitchMinWidth() + ", padding " + sw.getSwitchPadding() + ", thumb "
                + size(sw.getThumbDrawable()) + ", track " + size(sw.getTrackDrawable()));
        sw.setChecked(true);
        Log.i(TAG, "compound: switch set checked: " + sw.isChecked());

        CheckedTextView checked = (CheckedTextView) root.getChildAt(3);
        Log.i(TAG, "compound: checked text " + place(checked) + ", checked " + checked.isChecked() + ", mark "
                + size(checked.getCheckMarkDrawable()));
        checked.toggle();
        Log.i(TAG, "compound: checked text toggled: " + checked.isChecked());
    }

    /** RelativeLayout's placements in 1290 x 2000 px. */
    private static void relative(Context themed) {
        ViewGroup root = layout(themed, "relative", WIDTH, 2000);
        for (int i = 0; i < root.getChildCount(); i++) {
            Log.i(TAG, "relative: " + name(themed, root.getChildAt(i)) + " " + place(root.getChildAt(i)));
        }
    }

    /** GridLayout's and TableLayout's placements, and a Space. */
    private static void grid(Context themed) {
        ViewGroup root = layout(themed, "grid", WIDTH, 0);
        for (int i = 0; i < root.getChildCount(); i++) {
            View child = root.getChildAt(i);
            Log.i(TAG, "grid: " + name(themed, child) + " " + child.getClass().getName() + " " + place(child));
            if (child instanceof ViewGroup) {
                ViewGroup group = (ViewGroup) child;
                for (int j = 0; j < group.getChildCount(); j++) {
                    View cell = group.getChildAt(j);
                    Log.i(TAG, "grid: " + name(themed, cell) + " " + place(cell));
                    if (cell instanceof ViewGroup) {
                        ViewGroup row = (ViewGroup) cell;
                        for (int k = 0; k < row.getChildCount(); k++) {
                            Log.i(TAG, "grid: " + name(themed, row.getChildAt(k)) + " " + place(row.getChildAt(k)));
                        }
                    }
                }
            }
        }
    }

    /** ListView's and GridView's rows from ArrayAdapters with the platform's item layouts. */
    private static void lists(Context themed) {
        List<String> items = new ArrayList<>();
        for (int i = 1; i <= 20; i++) {
            items.add("Item " + i);
        }
        ListView list = new ListView(themed);
        list.setAdapter(new ArrayAdapter<>(themed, android.R.layout.simple_list_item_1, items));
        exactly(list, WIDTH, 1000);
        Log.i(TAG, "lists: list " + list.getChildCount() + " children, first " + list.getFirstVisiblePosition()
                + ", last " + list.getLastVisiblePosition() + ", divider " + list.getDividerHeight());
        rows("lists: list", list);
        list.setSelection(10);
        exactly(list, WIDTH, 1000);
        Log.i(TAG, "lists: list at 10: first " + list.getFirstVisiblePosition() + ", last "
                + list.getLastVisiblePosition() + ", first top " + list.getChildAt(0).getTop());

        GridView grid = new GridView(themed);
        grid.setNumColumns(3);
        grid.setVerticalSpacing(10);
        grid.setHorizontalSpacing(15);
        grid.setAdapter(new ArrayAdapter<>(themed, android.R.layout.simple_list_item_1, items.subList(0, 10)));
        exactly(grid, WIDTH, 1000);
        Log.i(TAG, "lists: grid " + grid.getChildCount() + " children, columns " + grid.getNumColumns()
                + ", column width " + grid.getColumnWidth());
        rows("lists: grid", grid);
    }

    /** ExpandableListView with two groups, the first expanded. */
    private static void expandable(Context themed) {
        List<Map<String, String>> groups = new ArrayList<>();
        groups.add(Collections.singletonMap("name", "A"));
        groups.add(Collections.singletonMap("name", "B"));
        List<List<Map<String, String>>> children = new ArrayList<>();
        List<Map<String, String>> first = new ArrayList<>();
        first.add(Collections.singletonMap("name", "A1"));
        first.add(Collections.singletonMap("name", "A2"));
        children.add(first);
        children.add(Collections.singletonList(Collections.singletonMap("name", "B1")));
        ExpandableListView list = new ExpandableListView(themed);
        String[] from = {"name"};
        int[] to = {android.R.id.text1};
        list.setAdapter(new SimpleExpandableListAdapter(themed, groups, android.R.layout.simple_expandable_list_item_1,
                from, to, children, android.R.layout.simple_list_item_1, from, to));
        list.expandGroup(0);
        exactly(list, WIDTH, 1500);
        Log.i(TAG, "expandable: " + list.getChildCount() + " children, group 0 expanded " + list.isGroupExpanded(0)
                + ", group 1 " + list.isGroupExpanded(1) + ", child 0/1 at flat position "
                + list.getFlatListPosition(ExpandableListView.getPackedPositionForChild(0, 1)));
        rows("expandable", list);
    }

    /** ViewFlipper's shown child and animations, a TextSwitcher and an ImageSwitcher. */
    private static void switchers(Context themed) {
        ViewFlipper flipper = (ViewFlipper) layout(themed, "switchers", WIDTH, 0);
        Animation in = flipper.getInAnimation();
        Log.i(TAG, "switchers: flipper " + place(flipper) + ", shown " + flipper.getDisplayedChild() + ", in "
                + in.getClass().getName() + " for " + in.getDuration() + " ms, flipping " + flipper.isFlipping());
        flipper.showNext();
        Log.i(TAG, "switchers: next: shown " + flipper.getDisplayedChild() + ", " + visibilities(flipper));
        flipper.setDisplayedChild(5);
        Log.i(TAG, "switchers: set 5: shown " + flipper.getDisplayedChild() + ", " + visibilities(flipper));

        TextSwitcher texts = new TextSwitcher(themed);
        texts.setFactory(() -> new TextView(themed));
        texts.setText("first");
        texts.setText("second");
        Log.i(TAG, "switchers: texts shows \"" + ((TextView) texts.getCurrentView()).getText() + "\", next \""
                + ((TextView) texts.getNextView()).getText() + "\"");

        ImageSwitcher images = new ImageSwitcher(themed);
        images.setFactory(() -> new ImageView(themed));
        images.setImageResource(themed.getResources().getIdentifier("box", "drawable", themed.getPackageName()));
        Drawable shown = ((ImageView) images.getCurrentView()).getDrawable();
        Log.i(TAG, "switchers: images shows " + shown.getClass().getName() + " " + size(shown));
    }

    /** A TextClock and a Chronometer whose texts do not change with the time, and an AnalogClock's size. */
    private static void clocks(Context themed) {
        ViewGroup root = layout(themed, "clocks", WIDTH, 0);
        for (int i = 0; i < root.getChildCount(); i++) {
            View clock = root.getChildAt(i);
            String text = clock instanceof TextView ? ", text \"" + ((TextView) clock).getText() + "\"" : "";
            Log.i(TAG, "clocks: " + name(themed, clock) + " " + clock.getClass().getName() + " " + place(clock)
                    + text);
        }
    }

    /** View animations at given times: alpha, translation, scale, rotation and a set. */
    private static void animations(Context themed) {
        Transformation out = new Transformation();
        AlphaAnimation alpha = new AlphaAnimation(1f, 0f);
        transform(alpha, 1000, 250, out);
        Log.i(TAG, "animations: alpha at 250 ms: alpha " + format(out.getAlpha()));
        TranslateAnimation translate = new TranslateAnimation(0, 300, 0, 120);
        transform(translate, 800, 400, out);
        Log.i(TAG, "animations: translate at 400 ms: " + values(out.getMatrix()));
        ScaleAnimation scale = new ScaleAnimation(1f, 2f, 1f, 0.5f, Animation.RELATIVE_TO_SELF, 0.5f,
                Animation.RELATIVE_TO_SELF, 0.5f);
        transform(scale, 1000, 500, out);
        Log.i(TAG, "animations: scale at 500 ms: " + values(out.getMatrix()));
        RotateAnimation rotate = new RotateAnimation(0f, 90f, 50f, 50f);
        transform(rotate, 600, 600, out);
        Log.i(TAG, "animations: rotate at 600 ms: " + values(out.getMatrix()));
        AnimationSet set = new AnimationSet(true);
        set.addAnimation(new AlphaAnimation(0f, 1f));
        set.addAnimation(new TranslateAnimation(0, 100, 0, 0));
        transform(set, 1000, 750, out);
        Log.i(TAG, "animations: set at 750 ms: alpha " + format(out.getAlpha()) + ", " + values(out.getMatrix()));
        Animation fade = AnimationUtils.loadAnimation(themed, android.R.anim.fade_in);
        Log.i(TAG, "animations: fade_in " + fade.getClass().getName() + " for " + fade.getDuration()
                + " ms, interpolator " + fade.getInterpolator().getClass().getName());
        LayoutAnimationController controller = new LayoutAnimationController(fade, 0.5f);
        Log.i(TAG, "animations: layout controller delay " + format(controller.getDelay()) + ", order "
                + controller.getOrder());
    }

    /** Interpolators at given inputs, the Material motion curves among them. */
    private static void interpolators(Context themed) {
        int[] ids = {android.R.interpolator.fast_out_slow_in, android.R.interpolator.linear_out_slow_in,
                android.R.interpolator.fast_out_linear_in, android.R.interpolator.accelerate_decelerate,
                android.R.interpolator.decelerate_cubic, android.R.interpolator.overshoot,
                android.R.interpolator.anticipate_overshoot, android.R.interpolator.bounce,
                android.R.interpolator.cycle};
        for (int id : ids) {
            Interpolator interpolator = AnimationUtils.loadInterpolator(themed, id);
            StringBuilder at = new StringBuilder();
            for (float input : new float[] {0f, 0.25f, 0.5f, 0.75f, 1f}) {
                at.append(' ').append(format(interpolator.getInterpolation(input)));
            }
            Log.i(TAG, "interpolators: " + themed.getResources().getResourceEntryName(id) + " "
                    + interpolator.getClass().getName() + ":" + at);
        }
    }

    // Helpers.

    private static ViewGroup layout(Context themed, String name, int width, int height) {
        int layout = themed.getResources().getIdentifier(name, "layout", themed.getPackageName());
        ViewGroup root = (ViewGroup) LayoutInflater.from(themed).inflate(layout, new LinearLayout(themed), false);
        if (height > 0) {
            exactly(root, width, height);
        } else {
            root.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                    View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
            root.layout(0, 0, root.getMeasuredWidth(), root.getMeasuredHeight());
        }
        root.jumpDrawablesToCurrentState();
        return root;
    }

    /** A view of the given minimum size, which an unbounded measure keeps. */
    private static View tall(Context themed, int width, int height) {
        View view = new View(themed);
        view.setMinimumWidth(width);
        view.setMinimumHeight(height);
        return view;
    }

    private static void exactly(View view, int width, int height) {
        view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
        view.layout(0, 0, width, height);
    }

    private static void transform(Animation animation, long duration, long time, Transformation out) {
        animation.setDuration(duration);
        animation.setStartTime(0);
        animation.initialize(200, 100, WIDTH, 2000);
        out.clear();
        animation.getTransformation(time, out);
    }

    private static void rows(String prefix, AbsListView list) {
        for (int i = 0; i < list.getChildCount(); i++) {
            View row = list.getChildAt(i);
            String text = row instanceof TextView ? " \"" + ((TextView) row).getText() + "\"" : "";
            Log.i(TAG, prefix + " row " + i + text + " " + place(row));
        }
    }

    private static String radios(RadioGroup group) {
        StringBuilder out = new StringBuilder("states");
        for (int i = 0; i < group.getChildCount(); i++) {
            out.append(' ').append(((RadioButton) group.getChildAt(i)).isChecked());
        }
        return out.toString();
    }

    private static String visibilities(ViewGroup group) {
        StringBuilder out = new StringBuilder("visibilities");
        for (int i = 0; i < group.getChildCount(); i++) {
            out.append(' ').append(group.getChildAt(i).getVisibility());
        }
        return out.toString();
    }

    private static String place(View view) {
        return "at " + view.getLeft() + ", " + view.getTop() + ", " + view.getWidth() + " x " + view.getHeight();
    }

    private static String size(Drawable drawable) {
        return drawable == null ? "none" : drawable.getIntrinsicWidth() + " x " + drawable.getIntrinsicHeight();
    }

    private static String values(Matrix matrix) {
        float[] values = new float[9];
        matrix.getValues(values);
        StringBuilder out = new StringBuilder("[");
        for (int i = 0; i < values.length; i++) {
            out.append(i == 0 ? "" : " ").append(format(values[i]));
        }
        return out.append(']').toString();
    }

    private static String format(float value) {
        return String.format(Locale.ROOT, "%.4f", value);
    }

    private static int id(Context themed, String name) {
        return themed.getResources().getIdentifier(name, "id", themed.getPackageName());
    }

    private static String name(Context themed, View view) {
        return view.getId() == View.NO_ID ? "(no id)" : themed.getResources().getResourceEntryName(view.getId());
    }

    private static String nameOf(Context themed, int id) {
        return id == View.NO_ID ? "none" : themed.getResources().getResourceEntryName(id);
    }
}
