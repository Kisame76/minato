// Hidden members of the public API's compile-only stub (ADR-0039 b3.4b), which tools/managergen joins with it:
// Color's color longs look their color space up by its ID.
class ColorSpace {
    /** @hide */
    static ColorSpace get(int index) {
        throw new RuntimeException("stub");
    }
}
