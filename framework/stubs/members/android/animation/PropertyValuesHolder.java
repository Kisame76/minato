// Hidden members of the public API's compile-only stub (ADR-0043 test 2), which tools/managergen joins with it:
// AnimatedVectorDrawable hands its animators' values to the render thread with them.
// (ADR-0039 step c2c2) LayoutTransition compares an animator's start and end keyframes.
class PropertyValuesHolder {
    /** @hide */
    public Class getValueType() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void getPropertyValues(PropertyValues values) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static class PropertyValues {
        public String propertyName;
        public Class type;
        public Object startValue;
        public Object endValue;
        public DataSource dataSource = null;

        public interface DataSource {
            Object getValueAtFraction(float fraction);
        }
    }

    /** @hide */
    Keyframes mKeyframes;
}
