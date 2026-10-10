// The written part of MediaMetadataRetriever (the Unity test APK gap sort, Formate's category 2, android.media as absent
// hardware), which tools/managergen joins with the API's methods. Minato has no media framework to decode with: a
// source that is not there is refused as Android refuses it (IllegalArgumentException from the path overload), any
// other source cannot be opened (the RuntimeException of a failed native setDataSource), and the queries answer null.
import android.content.Context;
import android.net.Uri;
import java.io.File;
import java.io.FileDescriptor;
import java.util.Map;

class MediaMetadataRetriever {
    public void setDataSource(String path) throws IllegalArgumentException {
        if (path == null || !new File(path).exists()) {
            throw new IllegalArgumentException();
        }
        throw new RuntimeException("setDataSource failed: status = 0x80000000");
    }

    public void setDataSource(String uri, Map<String, String> headers) throws IllegalArgumentException {
        throw new IllegalArgumentException();
    }

    public void setDataSource(FileDescriptor fd, long offset, long length) throws IllegalArgumentException {
        throw new RuntimeException("setDataSource failed: status = 0x80000000");
    }

    public void setDataSource(FileDescriptor fd) throws IllegalArgumentException {
        throw new RuntimeException("setDataSource failed: status = 0x80000000");
    }

    public void setDataSource(Context context, Uri uri) throws IllegalArgumentException, SecurityException {
        throw new IllegalArgumentException();
    }

    public void setDataSource(MediaDataSource dataSource) throws IllegalArgumentException {
        throw new RuntimeException("setDataSource failed: status = 0x80000000");
    }

    public String extractMetadata(int keyCode) {
        return null;
    }

    public void release() {
    }

    public void close() {
    }
}
