// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// ParcelFileDescriptor calls them (b3.3).
import java.io.FileDescriptor;
import java.io.IOException;

class MemoryFile {
    /** @hide */
    void deactivate() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public FileDescriptor getFileDescriptor() throws IOException {
        throw new RuntimeException("stub");
    }
}
