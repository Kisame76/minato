package io.github.kisame76.minato.test.drawables;

import android.app.Application;
import android.content.Context;
import android.content.res.TypedArray;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.RuntimeShader;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.AnimationDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.DrawableContainer;
import android.graphics.drawable.DrawableWrapper;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.RippleDrawable;
import android.util.Log;
import android.view.ContextThemeWrapper;
import java.nio.ByteBuffer;
import java.util.Locale;
import java.util.zip.CRC32;

/**
 * ADR-0043 step d)'s test, the XML drawables. On Theme.Material.Light, whose colors are fixed (Monet's stay out), it
 * inflates the app's res/drawable, one XML drawable for each tag that DrawableInflater knows, and a color state list as
 * a drawable; and the drawables that the theme's Button and CheckBox take, as their constructors read them from
 * buttonStyle and checkboxStyle (a ripple around an inset shape, an animated selector of vectors), so that the test
 * needs no widget classes. Each is drawn into a Bitmap on the CPU, as Android 15 draws a bitmap's canvas too, and
 * logged with the CRC-32 of its pixels (copyPixelsToBuffer: RGBA, premultiplied) and what its API answers, under the
 * tag drawables-test; then an AGSL RuntimeShader's compile, which the pressed ripple's shader needs too, and "done".
 * A failing part logs "failed" and its stack under drawables-test-device, which differs by device and is only shown.
 * tools/android-reference.sh takes the log from Android 15 on the M4 plan's display (density 480), and
 * tools/drawables-check.sh compares Minato's with it.
 */
public class DrawablesApplication extends Application {
    private static final String TAG = "drawables-test";
    private static final int SIZE = 48;
    private static final int[] ENABLED = {android.R.attr.state_enabled};
    private static final int[] PRESSED = {android.R.attr.state_enabled, android.R.attr.state_pressed};
    private static final int[] CHECKED = {android.R.attr.state_enabled, android.R.attr.state_checked};
    private static final String[] TAGS = {"ripple", "layer_list", "inset", "selector", "animated_selector",
            "animation_list", "level_list", "transition", "scale", "clip", "rotate", "animated_rotate", "adaptive_icon",
            "vector"};

    /** One part of the test, which may throw. */
    private interface Part {
        void run() throws Exception;
    }

    // In onCreate, as an app's code runs: AdaptiveIconDrawable reads the mask from the process's Application, which
    // ActivityThread gives out only after attachBaseContext.
    @Override
    public void onCreate() {
        super.onCreate();
        Context themed = new ContextThemeWrapper(this, android.R.style.Theme_Material_Light);
        part("button", () -> button(themed));
        part("checkbox", () -> checkBox(themed));
        for (String name : TAGS) {
            part(name, () -> tag(themed, name));
        }
        part("color", () -> color(themed));
        part("shader", DrawablesApplication::shader);
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

    /** The theme's Button background (buttonStyle's) at Material's smallest Button: idle, pressed and released. */
    private static void button(Context themed) {
        Drawable background = styled(themed, android.R.attr.buttonStyle, android.R.attr.background);
        Log.i(TAG, "button: background " + chain(background) + ", " + answers(background));
        RippleDrawable ripple = (RippleDrawable) background;
        Log.i(TAG, "button: ripple radius " + ripple.getRadius() + ", effect color "
                + hex(ripple.getEffectColor().getDefaultColor()));
        // 88 x 48 dp at density 480.
        background.setBounds(0, 0, 264, 144);
        background.setState(ENABLED);
        Log.i(TAG, "button: idle, drawn " + draw(background));
        // Android 15 makes the pressed ripple's shader, then skips the ripple on a canvas that is not hardware
        // accelerated: the pixels stay the idle ones on both.
        background.setHotspot(132, 72);
        background.setState(PRESSED);
        Log.i(TAG, "button: pressed, drawn " + draw(background));
        background.setState(ENABLED);
        background.jumpToCurrentState();
        Log.i(TAG, "button: released, drawn " + draw(background));
    }

    /** The theme's CheckBox button (checkboxStyle's), unchecked and checked, its transition ended. */
    private static void checkBox(Context themed) {
        Drawable button = styled(themed, android.R.attr.checkboxStyle, android.R.attr.button);
        Log.i(TAG, "checkbox: button " + chain(button) + ", " + answers(button));
        button.setBounds(0, 0, button.getIntrinsicWidth(), button.getIntrinsicHeight());
        button.setState(ENABLED);
        button.jumpToCurrentState();
        Log.i(TAG, "checkbox: unchecked " + chain(button) + ", drawn " + draw(button));
        button.setState(CHECKED);
        button.jumpToCurrentState();
        Log.i(TAG, "checkbox: checked " + chain(button) + ", drawn " + draw(button));
    }

    /** A widget style's drawable, as the widget's constructor reads it from the theme. */
    private static Drawable styled(Context themed, int style, int attribute) {
        TypedArray values = themed.obtainStyledAttributes(null, new int[] {attribute}, style, 0);
        try {
            return values.getDrawable(0);
        } finally {
            values.recycle();
        }
    }

    /** One tag's drawable from res/drawable: its classes and answers, its pixels, then its own change. */
    private static void tag(Context themed, String name) {
        int id = themed.getResources().getIdentifier(name, "drawable", themed.getPackageName());
        Drawable drawable = themed.getDrawable(id);
        Log.i(TAG, name + ": " + chain(drawable) + ", " + answers(drawable));
        drawable.setBounds(0, 0, SIZE, SIZE);
        Log.i(TAG, name + ": drawn " + draw(drawable));
        String change = change(name, drawable);
        if (change != null) {
            Log.i(TAG, name + ": " + change + ", " + chain(drawable) + ", drawn " + draw(drawable));
        }
    }

    /** Each tag's own change of state or level, its animation ended; null for none. */
    private static String change(String name, Drawable drawable) {
        switch (name) {
            case "ripple":
                // As the Button's: the shader is made, the ripple skipped on the CPU.
                drawable.setHotspot(SIZE / 2f, SIZE / 2f);
                drawable.setState(PRESSED);
                return "pressed";
            case "selector":
                drawable.setState(PRESSED);
                return "pressed";
            case "animated_selector":
                drawable.setState(CHECKED);
                drawable.jumpToCurrentState();
                return "checked";
            case "animation_list":
                AnimationDrawable frames = (AnimationDrawable) drawable;
                frames.selectDrawable(1);
                return "frame 1 of " + frames.getNumberOfFrames() + ", " + frames.getDuration(1) + " ms";
            case "level_list":
                drawable.setLevel(2);
                return "level 2";
            case "scale":
            case "clip":
            case "rotate":
                drawable.setLevel(5000);
                return "level 5000";
            case "adaptive_icon":
                RectF mask = new RectF();
                ((AdaptiveIconDrawable) drawable).getIconMask().computeBounds(mask, true);
                return "mask " + mask.toShortString();
            default:
                return null;
        }
    }

    /** A color state list as a drawable (Resources.getDrawable of res/color), default and pressed. */
    private static void color(Context themed) {
        int id = themed.getResources().getIdentifier("state_colors", "color", themed.getPackageName());
        Drawable drawable = themed.getDrawable(id);
        Log.i(TAG, "color: " + chain(drawable) + ", " + answers(drawable));
        drawable.setBounds(0, 0, SIZE, SIZE);
        Log.i(TAG, "color: drawn " + draw(drawable));
        drawable.setState(PRESSED);
        Log.i(TAG, "color: pressed, drawn " + draw(drawable));
    }

    /**
     * An AGSL shader, compiled and its uniforms set; drawn on the CPU, which Android 15 refuses for a RuntimeShader
     * (BaseCanvas), as for the pressed ripple's; then a program that does not compile, with Skia's message.
     */
    private static void shader() {
        RuntimeShader shader = new RuntimeShader("uniform float2 size;\n"
                + "uniform half4 tint;\n"
                + "half4 main(float2 p) {\n"
                + "    float2 q = p / size;\n"
                + "    return half4(q.x * tint.r, q.y * tint.g, (1.0 - q.x) * tint.b, 1.0);\n"
                + "}\n");
        shader.setFloatUniform("size", SIZE, SIZE);
        shader.setFloatUniform("tint", 1.0f, 0.75f, 0.5f, 1.0f);
        Paint paint = new Paint();
        paint.setShader(shader);
        Log.i(TAG, "shader: compiled, uniforms set");
        try {
            new Canvas(Bitmap.createBitmap(SIZE, SIZE, Bitmap.Config.ARGB_8888)).drawPaint(paint);
            Log.i(TAG, "shader: drawn on the CPU");
        } catch (IllegalArgumentException e) {
            Log.i(TAG, "shader: on the CPU: " + e.getMessage());
        }
        try {
            new RuntimeShader("half4 main(float2 p) { return undefined; }");
            Log.i(TAG, "shader: the wrong program compiled");
        } catch (IllegalArgumentException e) {
            Log.i(TAG, "shader: the wrong program: " + e.getMessage().trim());
        }
    }

    /** A drawable's classes: a layer list's layers, a wrapper's drawable, a container's current one. */
    private static String chain(Drawable drawable) {
        if (drawable == null) {
            return "null";
        }
        String name = drawable.getClass().getSimpleName();
        if (drawable instanceof LayerDrawable) {
            LayerDrawable layers = (LayerDrawable) drawable;
            StringBuilder text = new StringBuilder(name).append('[');
            for (int i = 0; i < layers.getNumberOfLayers(); i++) {
                text.append(i == 0 ? "" : ", ").append(chain(layers.getDrawable(i)));
            }
            return text.append(']').toString();
        }
        if (drawable instanceof DrawableWrapper) {
            return name + "[" + chain(((DrawableWrapper) drawable).getDrawable()) + "]";
        }
        if (drawable instanceof DrawableContainer) {
            return name + "(" + chain(drawable.getCurrent()) + ")";
        }
        if (drawable instanceof AdaptiveIconDrawable) {
            AdaptiveIconDrawable icon = (AdaptiveIconDrawable) drawable;
            return name + "[" + chain(icon.getBackground()) + ", " + chain(icon.getForeground()) + "]";
        }
        return name;
    }

    private static String answers(Drawable drawable) {
        Rect padding = new Rect();
        boolean padded = drawable.getPadding(padding);
        return "intrinsic " + drawable.getIntrinsicWidth() + "x" + drawable.getIntrinsicHeight() + ", padding "
                + (padded ? padding.toShortString() : "none") + ", opacity " + drawable.getOpacity() + ", stateful "
                + drawable.isStateful();
    }

    private static String draw(Drawable drawable) {
        Rect bounds = drawable.getBounds();
        Bitmap bitmap = Bitmap.createBitmap(bounds.width(), bounds.height(), Bitmap.Config.ARGB_8888);
        drawable.draw(new Canvas(bitmap));
        return crc(bitmap);
    }

    private static String crc(Bitmap bitmap) {
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
