// Hidden members of the public API's compile-only stub (ADR-0045 gap 1, batch 2), which tools/managergen joins with
// it: NotificationChannel's restore, the system's, looks a sound up again.
import android.content.ContentResolver;
import android.net.Uri;
import java.io.FileNotFoundException;

class RingtoneManager {
    /** @hide */
    public static final String EXTRA_RINGTONE_AUDIO_ATTRIBUTES_FLAGS =
            "android.intent.extra.ringtone.AUDIO_ATTRIBUTES_FLAGS";

    /** @hide MediaPlayer's (ADR-0060 e2e1), for a default sound's URI: AOSP's body. */
    public static Uri getCacheForType(int type, int userId) {
        if ((type & TYPE_RINGTONE) != 0) {
            return android.content.ContentProvider.maybeAddUserId(
                    android.provider.Settings.System.RINGTONE_CACHE_URI, userId);
        } else if ((type & TYPE_NOTIFICATION) != 0) {
            return android.content.ContentProvider.maybeAddUserId(
                    android.provider.Settings.System.NOTIFICATION_SOUND_CACHE_URI, userId);
        } else if ((type & TYPE_ALARM) != 0) {
            return android.content.ContentProvider.maybeAddUserId(
                    android.provider.Settings.System.ALARM_ALERT_CACHE_URI, userId);
        }
        return null;
    }

    /** @hide */
    public static Uri getRingtoneUriForRestore(ContentResolver contentResolver, String value, int ringtoneType)
            throws FileNotFoundException, IllegalArgumentException {
        throw new RuntimeException("stub");
    }
}
