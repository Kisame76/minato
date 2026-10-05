// Hidden members of the public API's compile-only stub (ADR-0043 step d)), which tools/managergen joins with it: the two
// default methods TextView's translation callback overrides.
class ViewTranslationCallback {
    /** @hide */
    default void enableContentPadding() {
    }

    /** @hide */
    default void setAnimationDurationMillis(int durationMillis) {
    }
}
