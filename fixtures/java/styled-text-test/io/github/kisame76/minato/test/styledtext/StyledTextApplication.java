package io.github.kisame76.minato.test.styledtext;

import android.app.Application;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.LocaleList;
import android.text.Annotation;
import android.text.ParcelableSpan;
import android.text.Spanned;
import android.text.style.AbsoluteSizeSpan;
import android.text.style.BackgroundColorSpan;
import android.text.style.BulletSpan;
import android.text.style.ForegroundColorSpan;
import android.text.style.LineBreakConfigSpan;
import android.text.style.RelativeSizeSpan;
import android.text.style.StyleSpan;
import android.text.style.TextAppearanceSpan;
import android.text.style.TypefaceSpan;
import android.text.style.URLSpan;
import android.util.DisplayMetrics;
import android.util.Log;
import java.io.IOException;
import java.net.URL;

/**
 * ADR-0039 b3.4b's test: the app's styled strings (res/values/strings.xml) through Resources.getText, each with every
 * span StringBlock gives it, logged with the tag styled-text-test and compared with Android 15's log
 * (tools/android-reference.sh --app styled-text-test). Each side reports once. Here the static initializer reports
 * through Resources over the APK, built as resources-test builds them, since step a's driver creates no Application.
 * On Android that path is hidden API, which ART blocks at targetSdk 35 (ApkAssets.loadFromPath throws
 * NoSuchMethodError), and onCreate reports through the app's own Resources; ours does not call onCreate before step c.
 */
public class StyledTextApplication extends Application {
    private static final String TAG = "styled-text-test";
    private static final String PACKAGE = "io.github.kisame76.minato.test.styledtext";
    private static final String[] STRINGS = {"phrase", "font", "link", "list", "breaks"};

    static {
        Resources res = null;
        try {
            res = apkResources();
        } catch (NoSuchMethodError e) {
            // Android: the hidden API is blocked, and onCreate reports.
        } catch (IOException e) {
            Log.i(TAG, "failed: " + e);
        }
        if (res != null) {
            run(res);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        run(getResources());
    }

    static void run(Resources res) {
        try {
            report(res);
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
        }
        Log.i(TAG, "done");
    }

    static void report(Resources res) {
        for (String name : STRINGS) {
            CharSequence text = res.getText(res.getIdentifier(name, "string", PACKAGE));
            Log.i(TAG, name + " " + text.getClass().getName() + " \"" + text.toString().replace("\n", "\\n") + "\"");
            if (!(text instanceof Spanned)) {
                continue;
            }
            Spanned spanned = (Spanned) text;
            for (Object span : spanned.getSpans(0, spanned.length(), Object.class)) {
                Log.i(TAG, name + " span " + spanned.getSpanStart(span) + "-" + spanned.getSpanEnd(span) + " flags 0x"
                        + Integer.toHexString(spanned.getSpanFlags(span)) + " " + describe(span));
            }
        }
    }

    /** The span's class, its span type if it has one, and what its public getters say. */
    static String describe(Object span) {
        StringBuilder line = new StringBuilder(span.getClass().getName());
        if (span instanceof ParcelableSpan) {
            line.append(" type ").append(((ParcelableSpan) span).getSpanTypeId());
        }
        if (span instanceof StyleSpan) {
            StyleSpan style = (StyleSpan) span;
            line.append(" style ").append(style.getStyle()).append(", fontWeightAdjustment ")
                    .append(style.getFontWeightAdjustment());
        } else if (span instanceof TypefaceSpan) {
            line.append(" family ").append(((TypefaceSpan) span).getFamily());
        } else if (span instanceof RelativeSizeSpan) {
            line.append(" sizeChange ").append(((RelativeSizeSpan) span).getSizeChange());
        } else if (span instanceof AbsoluteSizeSpan) {
            AbsoluteSizeSpan size = (AbsoluteSizeSpan) span;
            line.append(" size ").append(size.getSize()).append(", dip ").append(size.getDip());
        } else if (span instanceof ForegroundColorSpan) {
            line.append(" color #").append(Integer.toHexString(((ForegroundColorSpan) span).getForegroundColor()));
        } else if (span instanceof BackgroundColorSpan) {
            line.append(" color #").append(Integer.toHexString(((BackgroundColorSpan) span).getBackgroundColor()));
        } else if (span instanceof URLSpan) {
            line.append(" url ").append(((URLSpan) span).getURL());
        } else if (span instanceof Annotation) {
            Annotation annotation = (Annotation) span;
            line.append(' ').append(annotation.getKey()).append('=').append(annotation.getValue());
        } else if (span instanceof BulletSpan) {
            BulletSpan bullet = (BulletSpan) span;
            line.append(" gapWidth ").append(bullet.getGapWidth()).append(", bulletRadius ")
                    .append(bullet.getBulletRadius()).append(", color ").append(bullet.getColor())
                    .append(", leadingMargin ").append(bullet.getLeadingMargin(true));
        } else if (span instanceof TextAppearanceSpan) {
            TextAppearanceSpan appearance = (TextAppearanceSpan) span;
            line.append(" family ").append(appearance.getFamily()).append(", style ")
                    .append(appearance.getTextStyle()).append(", size ").append(appearance.getTextSize())
                    .append(", color ").append(appearance.getTextColor()).append(", linkColor ")
                    .append(appearance.getLinkTextColor());
        } else if (span instanceof LineBreakConfigSpan) {
            line.append(' ').append(((LineBreakConfigSpan) span).getLineBreakConfig());
        } else if (span instanceof Enum) {
            line.append(' ').append(span);
        }
        return line.toString();
    }

    /**
     * The app's Resources as ResourcesManager builds an app's, at decision 9's density 480 in en-US: its ApkAssets
     * beside the system's.
     */
    @SuppressWarnings("deprecation") // Resources(AssetManager, DisplayMetrics, Configuration), as in resources-test.
    static Resources apkResources() throws IOException {
        ApkAssets apk = ApkAssets.loadFromPath(apkPath());
        AssetManager assets = new AssetManager();
        assets.setApkAssets(new ApkAssets[] {apk}, false);
        DisplayMetrics metrics = new DisplayMetrics();
        metrics.setToDefaults();
        metrics.densityDpi = 480;
        metrics.density = 3f;
        metrics.scaledDensity = 3f;
        Configuration config = new Configuration();
        config.setToDefaults();
        config.densityDpi = 480;
        config.setLocales(LocaleList.forLanguageTags("en-US"));
        return new Resources(assets, metrics, config);
    }

    /** base.apk's path, from the class loader's URL of the manifest: jar:file:<path>!/AndroidManifest.xml. */
    static String apkPath() {
        URL manifest = StyledTextApplication.class.getClassLoader().getResource("AndroidManifest.xml");
        String url = String.valueOf(manifest);
        return url.substring("jar:file:".length(), url.indexOf("!/"));
    }
}
