// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View's drag shadow surface; RotationUtils turns a surface with a matrix.
// (ADR-0041 a)) DisplayInfo and Display read a display's refresh-rate range, which the local display service
// leaves out.
class SurfaceControl {
    /** @hide AttachedSurfaceControl.getBufferTransformHint's IntDef (ADR-0039 step c2c3). */
    public @interface BufferTransform {
    }

    /** @hide The layer that Surface.copyFrom takes over, which ADR-0041 a)'s surfaces give. */
    public long mNativeObject;

    public static class Transaction {
        /** @hide */
        public Transaction setMatrix(SurfaceControl sc, float dsdx, float dtdx, float dtdy, float dsdy) {
            throw new RuntimeException("stub");
        }

        /** @hide */
        public void apply(boolean sync) {
            throw new RuntimeException("stub");
        }
    }

    public static class Builder {
        /** @hide */
        public Builder(SurfaceSession session) {
            throw new RuntimeException("stub");
        }

        /** @hide */
        public Builder setCallsite(String callsite) {
            throw new RuntimeException("stub");
        }
    }

    /** @hide */
    public static final class RefreshRateRange implements android.os.Parcelable {
        public static final android.os.Parcelable.Creator<RefreshRateRange> CREATOR = null;

        @Override
        public int describeContents() {
            throw new RuntimeException("stub");
        }

        @Override
        public void writeToParcel(android.os.Parcel dest, int flags) {
            throw new RuntimeException("stub");
        }
    }

    /** @hide */
    public void setUnreleasedWarningCallSite(String callsite) {
        throw new RuntimeException("stub");
    }
}
