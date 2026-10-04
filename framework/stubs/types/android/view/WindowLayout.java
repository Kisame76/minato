package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: InsetsState's display cutout safe area starts from
 * its bounds. javac inlines them; the values are the tag's.
 */
public class WindowLayout {
    static final int MIN_X = -100000;

    static final int MIN_Y = -100000;

    static final int MAX_X = 100000;

    static final int MAX_Y = 100000;
}
