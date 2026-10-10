package android.util;

/**
 * A twin of the framework's android.util.Pair, compile-only (D8 leaves it out), with members the real one lacks: at
 * run time the app's references to them meet the framework's class.
 */
public class Pair<F, S> {
    public static int minatoMissingStatic;
    public int minatoMissingField;

    public Pair(F first, S second) {
    }

    public static int minatoMissing() {
        return 0;
    }
}
