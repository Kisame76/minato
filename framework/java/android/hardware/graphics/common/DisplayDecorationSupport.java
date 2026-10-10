/*
 * The display decoration parcelable of hardware/interfaces' graphics.common AIDL (the Unity test APK retest 11: Display's methods
 * name it, and a class whose method types cannot be resolved cannot be read by reflection): our class under its name
 * with the fields and parcel layout aidl writes for a structured parcelable, since its @VintfStability AIDL needs the
 * stable build that tools/build-framework.sh does not run. Minato's displays report no decoration support
 * (Display.getDisplayDecorationSupport answers null), so none is ever made but by an app.
 */
package android.hardware.graphics.common;

import android.os.Parcel;
import android.os.Parcelable;

/** @hide */
public class DisplayDecorationSupport implements Parcelable {
    /** A PixelFormat value. */
    public int format = 0;

    /** An AlphaInterpretation value. */
    public int alphaInterpretation = 0;

    public static final Parcelable.Creator<DisplayDecorationSupport> CREATOR =
            new Parcelable.Creator<DisplayDecorationSupport>() {
                @Override
                public DisplayDecorationSupport createFromParcel(Parcel in) {
                    DisplayDecorationSupport support = new DisplayDecorationSupport();
                    support.readFromParcel(in);
                    return support;
                }

                @Override
                public DisplayDecorationSupport[] newArray(int size) {
                    return new DisplayDecorationSupport[size];
                }
            };

    @Override
    public final void writeToParcel(Parcel out, int flags) {
        int start = out.dataPosition();
        out.writeInt(0);
        out.writeInt(format);
        out.writeInt(alphaInterpretation);
        int end = out.dataPosition();
        out.setDataPosition(start);
        out.writeInt(end - start);
        out.setDataPosition(end);
    }

    /** Reads the fields as aidl's code does: a size first, and only the fields the size covers. */
    public final void readFromParcel(Parcel in) {
        int start = in.dataPosition();
        int size = in.readInt();
        try {
            if (size < 4) {
                throw new android.os.BadParcelableException("Parcelable too small");
            }
            if (in.dataPosition() - start < size) {
                format = in.readInt();
            }
            if (in.dataPosition() - start < size) {
                alphaInterpretation = in.readInt();
            }
        } finally {
            if (start > Integer.MAX_VALUE - size) {
                throw new android.os.BadParcelableException("Overflow in the size of parcelable");
            }
            in.setDataPosition(start + size);
        }
    }

    @Override
    public int describeContents() {
        return 0;
    }
}
