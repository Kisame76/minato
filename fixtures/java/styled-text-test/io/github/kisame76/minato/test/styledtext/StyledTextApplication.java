package io.github.kisame76.minato.test.styledtext;

import android.app.Application;
import android.content.res.Resources;
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
import android.util.Log;

/**
 * ADR-0039 b3.4b's test: the app's styled strings (res/values/strings.xml) through Resources.getText, each with every
 * span StringBlock gives it, logged with the tag styled-text-test and compared with Android 15's log
 * (tools/android-reference.sh --app styled-text-test). onCreate reports through the app's own Resources, on Minato
 * since step c2b's ActivityThread calls it, and ends with "done".
 */
public class StyledTextApplication extends Application {
    private static final String TAG = "styled-text-test";
    private static final String PACKAGE = "io.github.kisame76.minato.test.styledtext";
    private static final String[] STRINGS = {"phrase", "font", "link", "list", "breaks"};

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
}
