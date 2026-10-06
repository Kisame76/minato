package io.github.kisame76.minato.test.checkbox;

import android.app.Application;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.os.Parcel;
import android.os.Parcelable;
import android.util.Log;
import android.util.SparseArray;
import android.view.ContextThemeWrapper;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import java.nio.ByteBuffer;
import java.util.Locale;
import java.util.zip.CRC32;

/**
 * ADR-0043 step e1)'s test, CompoundButton and CheckBox. On Theme.Material.Light, whose colors are fixed (Monet's stay
 * out), it inflates res/layout/boxes.xml, a checked CheckBox with a text and an unchecked one without, and makes
 * CheckBoxes in code, and logs under the tag checkbox-test what they answer: their state and its description, their
 * button drawable and its tint, their place, size and padding once laid out at the M4 plan's display (density 480),
 * the order of the listeners at performClick, setChecked with the value a CheckBox has and from inside its listener,
 * toggle, a saved state written to a Parcel and read back, and a CheckBox's pixels drawn on the CPU with their CRC-32,
 * as drawables-test logs its drawables'. Every drawable's transition is ended first (jumpDrawablesToCurrentState), so
 * that no frame of an animation enters the log. The test needs no window: as in drawables-test, the launcher activity
 * is the framework's own android.app.Activity and the driver loads only the Application class. A failing part logs
 * "failed" and its stack under checkbox-test-device, which differs by device and is only shown.
 * tools/android-reference.sh takes the log from Android 15 on the M4 plan's display, and tools/checkbox-check.sh
 * compares Minato's with it.
 */
public class CheckBoxApplication extends Application {
    private static final String TAG = "checkbox-test";
    /** The id under which saveHierarchyState keeps a CheckBox's state. */
    private static final int ID = 1;

    /** One part of the test, which may throw. */
    private interface Part {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Context themed = new ContextThemeWrapper(this, android.R.style.Theme_Material_Light);
        part("inflated", () -> inflated(themed));
        part("made", () -> made(themed));
        part("button", () -> button(themed));
        part("click", () -> click(themed));
        part("same", () -> same(themed));
        part("nested", () -> nested(themed));
        part("toggle", () -> toggle(themed));
        part("saved", () -> saved(themed));
        part("drawn", () -> drawn(themed));
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

    /** The layout's CheckBoxes, as LayoutInflater makes them from their tag. */
    private static void inflated(Context themed) {
        ViewGroup boxes = boxes(themed);
        for (int i = 0; i < boxes.getChildCount(); i++) {
            CheckBox box = (CheckBox) boxes.getChildAt(i);
            Log.i(TAG, "inflated: " + name(themed, box) + " " + box.getClass().getName() + ", " + state(box)
                    + ", clickable " + box.isClickable() + ", focusable " + box.isFocusable() + ", text \""
                    + box.getText() + "\"");
        }
    }

    /** A CheckBox made in code, which takes the theme's checkboxStyle. */
    private static void made(Context themed) {
        CheckBox box = new CheckBox(themed);
        Log.i(TAG, "made: " + state(box) + ", clickable " + box.isClickable() + ", focusable " + box.isFocusable()
                + ", text \"" + box.getText() + "\", accessibility class " + box.getAccessibilityClassName());
    }

    /** The layout's CheckBoxes laid out 1290 px wide: each one's button drawable and tint, place, size and padding. */
    private static void button(Context themed) {
        ViewGroup boxes = boxes(themed);
        boxes.measure(View.MeasureSpec.makeMeasureSpec(1290, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        boxes.layout(0, 0, boxes.getMeasuredWidth(), boxes.getMeasuredHeight());
        for (int i = 0; i < boxes.getChildCount(); i++) {
            CheckBox box = (CheckBox) boxes.getChildAt(i);
            String name = name(themed, box);
            box.jumpDrawablesToCurrentState();
            Drawable button = box.getButtonDrawable();
            Log.i(TAG, "button: " + name + " " + button.getClass().getName() + ", current "
                    + button.getCurrent().getClass().getName() + ", " + button.getIntrinsicWidth() + " x "
                    + button.getIntrinsicHeight() + ", its state checked " + has(button.getState()));
            ColorStateList tint = box.getButtonTintList();
            Log.i(TAG, "button: " + name + " tint " + (tint == null ? "none"
                    : hex(tint.getColorForState(box.getDrawableState(), 0)) + " of " + hex(tint.getDefaultColor()))
                    + ", mode " + box.getButtonTintMode());
            Log.i(TAG, "size: " + name + " at " + box.getLeft() + ", " + box.getTop() + ", " + box.getWidth() + " x "
                    + box.getHeight() + ", padding " + box.getPaddingLeft() + " " + box.getPaddingTop() + " "
                    + box.getPaddingRight() + " " + box.getPaddingBottom() + ", compound padding left "
                    + box.getCompoundPaddingLeft() + ", baseline " + box.getBaseline());
        }
    }

    /** performClick on an unchecked CheckBox: the order of its listeners, then once more without a click listener. */
    private static void click(Context themed) {
        CheckBox box = new CheckBox(themed);
        box.setOnCheckedChangeListener((button, checked) ->
                Log.i(TAG, "click: checked changed to " + checked + ", isChecked " + button.isChecked()));
        box.setOnClickListener(view -> Log.i(TAG, "click: clicked, isChecked " + ((CheckBox) view).isChecked()));
        boolean handled = box.performClick();
        Log.i(TAG, "click: handled " + handled + ", " + state(box));
        box.setOnClickListener(null);
        handled = box.performClick();
        Log.i(TAG, "click: without a click listener, handled " + handled + ", " + state(box));
    }

    /** setChecked with the value the CheckBox has: its listener does not run. */
    private static void same(Context themed) {
        CheckBox box = new CheckBox(themed);
        box.setChecked(true);
        int[] calls = {0};
        box.setOnCheckedChangeListener((button, checked) -> calls[0]++);
        box.setChecked(true);
        Log.i(TAG, "same: listener calls " + calls[0] + ", " + state(box));
    }

    /** A listener that sets its CheckBox back: the inner setChecked changes the state without the listener again. */
    private static void nested(Context themed) {
        CheckBox box = new CheckBox(themed);
        int[] calls = {0};
        box.setOnCheckedChangeListener((button, checked) -> {
            calls[0]++;
            Log.i(TAG, "nested: listener saw " + checked);
            button.setChecked(!checked);
        });
        box.setChecked(true);
        Log.i(TAG, "nested: listener calls " + calls[0] + ", " + state(box));
    }

    private static void toggle(Context themed) {
        CheckBox box = new CheckBox(themed);
        box.toggle();
        Log.i(TAG, "toggle: " + state(box));
        box.toggle();
        Log.i(TAG, "toggle: again " + state(box));
    }

    /** A checked CheckBox's saved state, written to a Parcel and read back, restores an unchecked one. */
    private static void saved(Context themed) {
        CheckBox box = new CheckBox(themed);
        box.setId(ID);
        box.setChecked(true);
        SparseArray<Parcelable> saved = new SparseArray<>();
        box.saveHierarchyState(saved);
        Parcelable state = saved.get(ID);
        Parcelable back;
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeParcelable(state, 0);
            parcel.setDataPosition(0);
            back = parcel.readParcelable(CompoundButton.class.getClassLoader());
        } finally {
            parcel.recycle();
        }
        CheckBox restored = new CheckBox(themed);
        restored.setId(ID);
        SparseArray<Parcelable> read = new SparseArray<>();
        read.put(ID, back);
        restored.restoreHierarchyState(read);
        Log.i(TAG, "saved: " + state.getClass().getName() + ", read back as " + back.getClass().getName()
                + ", restored " + state(restored));
    }

    /** A CheckBox without a text at its own size, drawn on the CPU unchecked and checked. */
    private static void drawn(Context themed) {
        CheckBox box = new CheckBox(themed);
        int unspecified = View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED);
        box.measure(unspecified, unspecified);
        box.layout(0, 0, box.getMeasuredWidth(), box.getMeasuredHeight());
        box.jumpDrawablesToCurrentState();
        Log.i(TAG, "drawn: unchecked " + draw(box));
        box.setChecked(true);
        box.jumpDrawablesToCurrentState();
        Log.i(TAG, "drawn: checked " + draw(box));
    }

    private static ViewGroup boxes(Context themed) {
        int layout = themed.getResources().getIdentifier("boxes", "layout", themed.getPackageName());
        return (ViewGroup) LayoutInflater.from(themed).inflate(layout, null);
    }

    private static String name(Context themed, View view) {
        return themed.getResources().getResourceEntryName(view.getId());
    }

    private static String state(CompoundButton box) {
        return "checked " + box.isChecked() + " (\"" + box.getStateDescription() + "\")";
    }

    private static boolean has(int[] state) {
        for (int value : state) {
            if (value == android.R.attr.state_checked) {
                return true;
            }
        }
        return false;
    }

    private static String draw(View view) {
        Bitmap bitmap = Bitmap.createBitmap(view.getWidth(), view.getHeight(), Bitmap.Config.ARGB_8888);
        view.draw(new Canvas(bitmap));
        ByteBuffer pixels = ByteBuffer.allocate(bitmap.getByteCount());
        bitmap.copyPixelsToBuffer(pixels);
        CRC32 crc = new CRC32();
        crc.update(pixels.array());
        return String.format(Locale.ROOT, "%dx%d crc %08x", bitmap.getWidth(), bitmap.getHeight(), crc.getValue());
    }

    private static String hex(int color) {
        return String.format(Locale.ROOT, "%08x", color);
    }
}
