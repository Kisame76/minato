// The written part of SurfaceControl (App, the Unity test APK retest 11: a class apps reflect over names it, so it ships as a
// placeholder: every body logs once and throws UnsupportedOperationException (Grafik owns it, replaced by ADR-0058 b) /
// category 2)), which tools/managergen joins with the API's methods. Before: the hidden members of the public API's
// compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it: View's drag shadow surface;
// RotationUtils turns a surface with a matrix. (ADR-0041 a)) DisplayInfo and Display read a display's refresh-rate
// range, which the local display service leaves out.
class SurfaceControl {
    /** @hide AttachedSurfaceControl.getBufferTransformHint's IntDef (ADR-0039 step c2c3). */
    public @interface BufferTransform {
    }

    /** @hide The layer that Surface.copyFrom takes over, which ADR-0041 a)'s surfaces give. */
    public long mNativeObject;

    public static class Transaction {
        /** @hide */
        public Transaction setMatrix(SurfaceControl sc, float dsdx, float dtdx, float dtdy, float dsdy) {
            throw Unwritten.placeholder(
                    "android.view.SurfaceControl.setMatrix(SurfaceControl,float,float,float,float)");
        }

        /** @hide */
        public void apply(boolean sync) {
            throw Unwritten.placeholder("android.view.SurfaceControl.apply(boolean)");
        }
    }

    public static class Builder {
        /** @hide */
        public Builder(SurfaceSession session) {
            throw Unwritten.placeholder("android.view.SurfaceControl.Builder(SurfaceSession)");
        }

        /** @hide */
        public Builder setCallsite(String callsite) {
            throw Unwritten.placeholder("android.view.SurfaceControl.setCallsite(String)");
        }
    }

    /** @hide */
    public static final class RefreshRateRange implements android.os.Parcelable {
        public static final android.os.Parcelable.Creator<RefreshRateRange> CREATOR = null;

        @Override
        public int describeContents() {
            throw Unwritten.placeholder("android.view.SurfaceControl.describeContents()");
        }

        @Override
        public void writeToParcel(android.os.Parcel dest, int flags) {
            throw Unwritten.placeholder("android.view.SurfaceControl.writeToParcel(android.os.Parcel,int)");
        }
    }

    /** @hide */
    public void setUnreleasedWarningCallSite(String callsite) {
        throw Unwritten.placeholder("android.view.SurfaceControl.setUnreleasedWarningCallSite(String)");
    }
}
