// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// XmlUtils calls them.
import com.android.modules.utils.TypedXmlPullParser;
import com.android.modules.utils.TypedXmlSerializer;

class Xml {
    /** @hide */
    public static TypedXmlPullParser newFastPullParser() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static TypedXmlSerializer newFastSerializer() {
        throw new RuntimeException("stub");
    }
}
