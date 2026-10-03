package android.util;

/**
 * Compile-only stub (ADR-0043 test 2), never in framework.jar: AnimatedVectorDrawable's render thread animator queues
 * its actions in it, which runs only with a render thread (decision 2).
 */
public class IntArray {
    public IntArray() {
        throw new RuntimeException("stub");
    }

    public void add(int value) {
        throw new RuntimeException("stub");
    }

    public int get(int index) {
        throw new RuntimeException("stub");
    }

    public int size() {
        throw new RuntimeException("stub");
    }

    public void clear() {
        throw new RuntimeException("stub");
    }
}
