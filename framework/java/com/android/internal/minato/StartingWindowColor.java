package com.android.internal.minato;

import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Rect;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.LayerDrawable;
import android.util.Log;

import com.android.internal.graphics.palette.Palette;
import com.android.internal.graphics.palette.Quantizer;
import com.android.internal.graphics.palette.VariationalKMeansQuantizer;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

/**
 * The starting window's colours (ADR-0076), as Android 15's WM Shell computes its splash screen's
 * (SplashscreenContentDrawer): the starting activity's theme's windowSplashScreenBackground when it is not
 * transparent, else its windowBackground drawable's colour (a ColorDrawable's own; any other drawable's dominant
 * colour, drawn at most 40 x 40, when at least half of its pixels are not transparent; a LayerDrawable by its bottom
 * layer), else Shell's splash_window_background_default: white by day, black by night. And the theme's
 * windowSplashScreenIconBackgroundColor, transparent when it names none. The host draws them before the process can
 * (the colours go to it with APKRT_REQUEST_STARTING_WINDOW), so they are computed in the app's process at its start.
 * A splash theme that an activity set (SplashScreen.setSplashScreenTheme) takes the place of the activity's theme at
 * the later starts, as Android's package manager keeps it: by its resource name, in the app's data folder.
 */
public final class StartingWindowColor {
    /** Shell's splash_window_background_default by day (values) and by night (values-night). */
    static final int DEFAULT_DAY = 0xFFFFFFFF;
    static final int DEFAULT_NIGHT = 0xFF000000;
    /** Shell's MAX_BITMAP_SIZE for a drawable's colour. */
    private static final int MAX_BITMAP_SIZE = 40;
    /** The kept splash theme's name, in the app's data folder. */
    private static final String THEME_FILE = "splash-screen-theme";

    private StartingWindowColor() {
    }

    /**
     * The splash colour and the icon background colour (0 for none) of `info`'s theme, else the application's, else
     * Theme.DeviceDefault.DayNight, as Shell's getSplashScreenTheme, in `context`'s configuration.
     */
    public static int[] of(Context context, ActivityInfo info) {
        int theme = keptTheme(context);
        if (theme == 0) {
            theme = info.getThemeResource();
        }
        if (theme == 0) {
            theme = com.android.internal.R.style.Theme_DeviceDefault_DayNight;
        }
        Context themed = context.createConfigurationContext(context.getResources().getConfiguration());
        themed.setTheme(theme);
        TypedArray a = themed.obtainStyledAttributes(com.android.internal.R.styleable.Window);
        int backgroundId = a.getResourceId(com.android.internal.R.styleable.Window_windowBackground, 0);
        int splash = a.getColor(com.android.internal.R.styleable.Window_windowSplashScreenBackground,
                Color.TRANSPARENT);
        int icon = a.getColor(com.android.internal.R.styleable.Window_windowSplashScreenIconBackgroundColor,
                Color.TRANSPARENT);
        a.recycle();
        int fallback = context.getResources().getConfiguration().isNightModeActive() ? DEFAULT_NIGHT : DEFAULT_DAY;
        Drawable drawable = null;
        if (splash != Color.TRANSPARENT) {
            drawable = new ColorDrawable(splash);
        } else if (backgroundId != 0) {
            drawable = themed.getDrawable(backgroundId);
        }
        return new int[] {estimate(drawable, fallback), icon};
    }

    /**
     * Keeps `themeId`'s name as the app's splash theme for its later starts, or forgets it for 0 (Resources.ID_NULL),
     * as SplashScreenImpl hands it to Android's package manager; a theme that is not the app's is not kept.
     */
    public static void keepTheme(Context context, int themeId) {
        File file = new File(context.getDataDir(), THEME_FILE);
        try {
            if (themeId == Resources.ID_NULL) {
                Files.deleteIfExists(file.toPath());
            } else {
                String name = context.getResources().getResourceName(themeId);
                Files.write(file.toPath(), name.getBytes(StandardCharsets.UTF_8));
            }
        } catch (IOException | Resources.NotFoundException e) {
            Log.w("SplashScreen", "Couldn't persist the starting theme: " + e);
        }
    }

    /** The kept splash theme's ID in `context`'s resources, 0 for none or one that the app no longer has. */
    private static int keptTheme(Context context) {
        File file = new File(context.getDataDir(), THEME_FILE);
        if (!file.isFile()) {
            return 0;
        }
        try {
            String name = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            return context.getResources().getIdentifier(name, null, null);
        } catch (IOException e) {
            return 0;
        }
    }

    /** Shell's estimateWindowBGColor with its TRANSPARENT_FILTER. */
    static int estimate(Drawable drawable, int fallback) {
        if (drawable instanceof LayerDrawable layers && layers.getNumberOfLayers() > 0) {
            drawable = layers.getDrawable(0);
        }
        if (drawable == null) {
            return fallback;
        }
        if (drawable instanceof ColorDrawable color) {
            return Color.alpha(color.getColor()) / 255f < 0.5f ? fallback : color.getColor();
        }
        int width = drawable.getIntrinsicWidth();
        int height = drawable.getIntrinsicHeight();
        if (width <= 0 || height <= 0) {
            width = MAX_BITMAP_SIZE;
            height = MAX_BITMAP_SIZE;
        } else {
            width = Math.min(width, MAX_BITMAP_SIZE);
            height = Math.min(height, MAX_BITMAP_SIZE);
        }
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        Rect bounds = drawable.copyBounds();
        drawable.setBounds(0, 0, width, height);
        drawable.draw(new Canvas(bitmap));
        drawable.setBounds(bounds);
        AlphaFilterQuantizer quantizer = new AlphaFilterQuantizer();
        Palette palette = new Palette.Builder(bitmap, quantizer).maximumColorCount(5).generate();
        bitmap.recycle();
        if (quantizer.passRatio < 0.5f) {
            return fallback;
        }
        Palette.Swatch dominant = palette.getDominantSwatch();
        return dominant != null ? dominant.getInt() : Color.BLACK;
    }

    /**
     * Shell's AlphaFilterQuantizer with TRANSPARENT_FILTER: the pixels that are not fully transparent, from the last
     * down to the second, as Shell counts them, through VariationalKMeansQuantizer; their share of all pixels is the
     * pass ratio.
     */
    private static final class AlphaFilterQuantizer implements Quantizer {
        private final Quantizer mInner = new VariationalKMeansQuantizer();
        float passRatio;

        @Override
        public void quantize(int[] pixels, int maxColors) {
            int count = 0;
            for (int i = pixels.length - 1; i > 0; i--) {
                if (Color.alpha(pixels[i]) != 0) {
                    count++;
                }
            }
            passRatio = pixels.length == 0 ? 0f : (float) count / pixels.length;
            int[] samples = new int[count];
            int n = 0;
            for (int i = pixels.length - 1; i > 0; i--) {
                if (Color.alpha(pixels[i]) != 0) {
                    samples[n++] = pixels[i];
                }
            }
            mInner.quantize(samples, maxColors);
        }

        @Override
        public List<Palette.Swatch> getQuantizedColors() {
            return mInner.getQuantizedColors();
        }
    }
}
