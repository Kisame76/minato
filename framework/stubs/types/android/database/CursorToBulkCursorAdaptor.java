package android.database;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: a provider's cursor for a caller of another
 * process, which ContentProviderNative's IPC makes; no other process calls a provider on Minato.
 */
public final class CursorToBulkCursorAdaptor {
    public CursorToBulkCursorAdaptor(Cursor cursor, IContentObserver observer, String providerName) {
        throw new RuntimeException("stub");
    }

    public BulkCursorDescriptor getBulkCursorDescriptor() {
        throw new RuntimeException("stub");
    }

    public void close() {
        throw new RuntimeException("stub");
    }
}
