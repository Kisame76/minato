// Hidden members of the public API's compile-only stub (ADR-0045, gap 13a), which tools/managergen joins with it:
// Settings' configuration of a user asks for the system's grammatical gender when the terms-of-address flag is on.
class GrammaticalInflectionManager {
    /** @hide */
    public int peekSystemGrammaticalGenderByUserId(int userId) {
        throw new RuntimeException("stub");
    }
}
