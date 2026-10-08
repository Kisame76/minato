// Hidden members of the public API's compile-only stub (ADR-0045 gap 1, batch 2), which tools/managergen joins with
// it: NotificationChannel's restore, the system's, looks a sound up again.
import android.content.ContentResolver;
import android.net.Uri;
import java.io.FileNotFoundException;

class RingtoneManager {
    /** @hide */
    public static Uri getRingtoneUriForRestore(ContentResolver contentResolver, String value, int ringtoneType)
            throws FileNotFoundException, IllegalArgumentException {
        throw new RuntimeException("stub");
    }
}
