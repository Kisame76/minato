// The written part of MemoryFile (the Unity test APK retest 11: a class apps reflect over names it, so it ships as a
// placeholder: every body logs once and throws UnsupportedOperationException), which tools/managergen joins with the
// API's methods. Before: the hidden members of the public API's compile-only stub (ADR-0039 b3.1), which
// tools/managergen joins with it: ParcelFileDescriptor calls them (b3.3).
import java.io.FileDescriptor;
import java.io.IOException;

class MemoryFile {
    /** @hide */
    void deactivate() {
        throw Unwritten.placeholder("android.os.MemoryFile.deactivate()");
    }

    /** @hide */
    public FileDescriptor getFileDescriptor() throws IOException {
        throw Unwritten.placeholder("android.os.MemoryFile.getFileDescriptor()");
    }
}
