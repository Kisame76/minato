package android.view;

/**
 * Compile-only stub (ADR-0039 b3.1), never in framework.jar: Resources.Theme.encode names it; View, ViewGroup and
 * FrameLayout encode their properties with it for the view server (step c2c2).
 */
public class ViewHierarchyEncoder {
    public void beginObject(Object o) {
        throw new RuntimeException("stub");
    }

    public void addProperty(String name, String value) {
        throw new RuntimeException("stub");
    }

    public void endObject() {
        throw new RuntimeException("stub");
    }

    public void addProperty(String name, boolean v) {
        throw new RuntimeException("stub");
    }

    public void addProperty(String name, short s) {
        throw new RuntimeException("stub");
    }

    public void addProperty(String name, int v) {
        throw new RuntimeException("stub");
    }

    public void addProperty(String name, float v) {
        throw new RuntimeException("stub");
    }

    public void addPropertyKey(String name) {
        throw new RuntimeException("stub");
    }

    public void addUserProperty(String name, String s) {
        throw new RuntimeException("stub");
    }
}
