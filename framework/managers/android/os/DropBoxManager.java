// The written part of DropBoxManager (services batch E): the system's store of crash and event logs, of which Minato has
// none. What the emulator (apk-runner-35-bare) answers an app that holds neither READ_LOGS nor PEEK_DROPBOX
// [verified: services-e-test]: every tag is enabled, reading entries is a SecurityException. Adding an entry needs no
// permission on Android; here it is dropped, as no one reads the store.
import android.content.Context;

class DropBoxManager {
    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public DropBoxManager(Context context) {
    }

    public void addText(String tag, String data) {
    }

    public void addData(String tag, byte[] data, int flags) {
        if (data == null) throw new NullPointerException("data == null");
    }

    public void addFile(String tag, java.io.File file, int flags) throws java.io.IOException {
        if (file == null) throw new NullPointerException("file == null");
    }

    public boolean isTagEnabled(String tag) {
        return true;
    }

    public Entry getNextEntry(String tag, long msec) {
        throw new SecurityException("Neither user " + android.os.Process.myUid()
                + " nor current process has android.permission.READ_LOGS.");
    }
}
