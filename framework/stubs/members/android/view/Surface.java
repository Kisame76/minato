// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// WindowConfiguration names them.
class Surface {
    /** @hide */
    public @interface Rotation {
    }

    /** @hide */
    public static String rotationToString(int rotation) {
        throw new RuntimeException("stub");
    }
}
