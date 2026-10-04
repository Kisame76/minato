package android.database;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: a provider's cursor for a caller of another
 * process, which ContentProviderNative's IPC makes; no other process calls a provider on Minato.
 */
public final class BulkCursorToCursorAdaptor extends AbstractWindowedCursor {
    public IContentObserver getObserver() {
        throw new RuntimeException("stub");
    }

    public void initialize(BulkCursorDescriptor d) {
        throw new RuntimeException("stub");
    }

    public int getCount() {
        throw new RuntimeException("stub");
    }

    public String[] getColumnNames() {
        throw new RuntimeException("stub");
    }
}
