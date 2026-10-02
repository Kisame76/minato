package android.text.style;

import android.os.Parcel;

/**
 * Compile-only stub (ADR-0039 b3.4), never in framework.jar: TextUtils.CHAR_SEQUENCE_CREATOR makes one from a parcel
 * of styled text, which no app process of Minato receives yet.
 */
public class AccessibilityURLSpan {
    public AccessibilityURLSpan(Parcel p) {
        throw new RuntimeException("stub");
    }
}
