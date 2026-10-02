package android.graphics;

import android.os.Parcel;

/**
 * Compile-only stub (ADR-0039 b3.4b), never in framework.jar: TypefaceSpan and TextAppearanceSpan parcel their
 * Typeface with it, which no app process of Minato does yet. It goes when step c takes Typeface from AOSP.
 */
public class LeakyTypefaceStorage {
    public static void writeTypefaceToParcel(Typeface typeface, Parcel parcel) {
        throw new RuntimeException("stub");
    }

    public static Typeface readTypefaceFromParcel(Parcel parcel) {
        throw new RuntimeException("stub");
    }
}
