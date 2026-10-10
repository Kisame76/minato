// The written part of MediaExtractor (the Unity test APK gap sort, Formate's category 2, android.media as absent hardware),
// which tools/managergen joins with the API's methods. Minato has no media framework to read containers with: a source
// cannot be opened (the IOException of a failed native setDataSource), and without one the extractor has no tracks.
import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.net.Uri;
import java.io.FileDescriptor;
import java.io.IOException;
import java.util.Map;

class MediaExtractor {
    public final void setDataSource(Context context, Uri uri, Map<String, String> headers) throws IOException {
        throw new IOException();
    }

    public final void setDataSource(String path, Map<String, String> headers) throws IOException {
        throw new IOException();
    }

    public final void setDataSource(String path) throws IOException {
        throw new IOException();
    }

    public final void setDataSource(AssetFileDescriptor afd) throws IOException, IllegalArgumentException,
            IllegalStateException {
        throw new IOException();
    }

    public final void setDataSource(FileDescriptor fd) throws IOException {
        throw new IOException();
    }

    public final void setDataSource(FileDescriptor fd, long offset, long length) throws IOException {
        throw new IOException();
    }

    public final void setDataSource(MediaDataSource dataSource) throws IOException {
        throw new IOException();
    }

    public int getTrackCount() {
        return 0;
    }

    public final void release() {
    }
}
