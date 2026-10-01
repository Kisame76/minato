// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// Resources and ResourcesImpl call them.
import android.content.res.Resources;
import android.content.res.Resources.Theme;
import org.xmlpull.v1.XmlPullParser;

class Drawable {
    /** @hide */
    public void clearMutated() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static Drawable createFromXmlForDensity(Resources r, XmlPullParser parser, int density, Theme theme) {
        throw new RuntimeException("stub");
    }
}
