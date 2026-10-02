package android.text.style;

import android.os.Parcel;

/**
 * Compile-only stub (ADR-0039 b3.4), never in framework.jar: TextUtils.CHAR_SEQUENCE_CREATOR makes one from a parcel
 * of styled text, which no app process of Minato receives yet.
 */
public class AccessibilityClickableSpan {
    public AccessibilityClickableSpan(Parcel p) {
        throw new RuntimeException("stub");
    }
}
