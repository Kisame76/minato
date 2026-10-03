package android.graphics.drawable;

import android.content.res.Resources;
import android.util.AttributeSet;

import org.xmlpull.v1.XmlPullParser;

/**
 * Compile-only stub (ADR-0039 b3.1), never in framework.jar: Resources.getDrawableInflater makes it, and Drawable's
 * inflation from XML asks it (ADR-0043 test 2), which waits for step c's drawables.
 */
public class DrawableInflater {
    public DrawableInflater(Resources res, ClassLoader classLoader) {
        throw new RuntimeException("stub");
    }

    public Drawable inflateFromXmlForDensity(String name, XmlPullParser parser, AttributeSet attrs, int density,
            Resources.Theme theme) {
        throw new RuntimeException("stub");
    }
}
