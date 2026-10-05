// Hidden members of the public API's compile-only stub (ADR-0043 step d)), which tools/managergen joins with it:
// SelectionActionModeHelper's check of a selection's text; autoLink stays a gap (no libphonenumber).
class Linkify {
    /** @hide */
    public static boolean containsUnsupportedCharacters(String text) {
        throw new RuntimeException("stub");
    }
}
