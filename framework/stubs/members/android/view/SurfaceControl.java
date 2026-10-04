// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View's drag shadow surface; RotationUtils turns a surface with a matrix.
class SurfaceControl {
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
}
