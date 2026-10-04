package android.provider;

import android.content.Context;
import android.os.ParcelFileDescriptor;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: MediaProvider's module API, which Context's javadoc
 * links and FileUtils.convertToModernFd asks for the original media format; Minato has no MediaProvider. javac
 * inlines the constant, so it carries AOSP's value.
 */
public final class MediaStore {
    public static final String AUTHORITY = "media";

    public static ParcelFileDescriptor getOriginalMediaFormatFileDescriptor(Context context,
            ParcelFileDescriptor fileDescriptor) {
        throw new RuntimeException("stub");
    }
}
