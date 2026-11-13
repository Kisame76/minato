// Hidden members of the public API's compile-only stub (ADR-0045), which tools/managergen joins with
// it: DisplayManager's virtual displays set a unique ID and the display to mirror on the builder.
class VirtualDisplayConfig {
    public static final class Builder {
        /** @hide */
        public Builder setUniqueId(String uniqueId) {
            throw new RuntimeException("stub");
        }

        /** @hide */
        public Builder setDisplayIdToMirror(int displayIdToMirror) {
            throw new RuntimeException("stub");
        }
    }
}
