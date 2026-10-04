package android.provider;

import android.content.Context;
import android.os.ParcelFileDescriptor;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: MediaProvider's module API, which Context's javadoc
 * links and FileUtils.convertToModernFd asks for the original media format, and whose capture actions and file scan
 * Intent names (step c2c1); Minato has no MediaProvider. javac inlines the constants, so they carry AOSP's values.
 */
public final class MediaStore {
    public static final String AUTHORITY = "media";

    public static ParcelFileDescriptor getOriginalMediaFormatFileDescriptor(Context context,
            ParcelFileDescriptor fileDescriptor) {
        throw new RuntimeException("stub");
    }

    public static final String ACTION_IMAGE_CAPTURE = "android.media.action.IMAGE_CAPTURE";

    public static final String ACTION_IMAGE_CAPTURE_SECURE = "android.media.action.IMAGE_CAPTURE_SECURE";

    public static final String ACTION_VIDEO_CAPTURE = "android.media.action.VIDEO_CAPTURE";

    public static final String EXTRA_OUTPUT = "output";

    public static android.net.Uri scanFile(android.content.ContentResolver resolver, java.io.File file) {
        throw new RuntimeException("stub");
    }
}
