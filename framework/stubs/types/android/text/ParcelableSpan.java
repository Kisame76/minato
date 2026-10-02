package android.text;

import android.os.Parcel;
import android.os.Parcelable;

/**
 * Compile-only stub (ADR-0039 b3.4), never in framework.jar, in place of the public API's generated one: TextUtils
 * calls the two hidden methods and LineHeightSpan.Standard implements them. AOSP's interface declares them abstract,
 * which the generated stubs of the other public spans do not implement, so here they are default methods.
 */
public interface ParcelableSpan extends Parcelable {
    int getSpanTypeId();

    /** @hide */
    default int getSpanTypeIdInternal() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    default void writeToParcelInternal(Parcel dest, int flags) {
        throw new RuntimeException("stub");
    }
}
