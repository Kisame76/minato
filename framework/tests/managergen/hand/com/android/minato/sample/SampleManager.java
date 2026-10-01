// The written part of SampleManager: imports, then members in a class body. A member replaces the generated one
// with its name and parameter types; one marked @hide is added beside the API.
import static java.util.Objects.requireNonNull;

import java.util.ArrayList;
import java.util.List;

class SampleManager {
    private final List<String> calls = new ArrayList<>();
    private static final boolean READY;

    static {
        READY = true;
    }

    /** @hide Android's managers take their context; ours takes a name. */
    public SampleManager(String name) {
        calls.add("new " + requireNonNull(name) + (READY ? "" : "?"));
    }

    public int getMode() {
        calls.add("getMode");
        return 2; // a '}' and "}" in a comment and a string
    }

    /**
     * Replaces the generated overload with the listener; the other stays generated.
     */
    public int requestFocus(SampleManager.OnChangeListener listener, int stream, int hint) {
        listener.onChange(hint);
        return 1;
    }

    /** Its hidden member joins the generated nested class, whose API members stay. */
    public static final class Settings {
        /** @hide What AOSP's classes call on the nested class. */
        public int hidden() {
            return 3;
        }
    }

    /** @hide What AOSP's classes call. */
    public List<String> calls() {
        return calls;
    }
}
