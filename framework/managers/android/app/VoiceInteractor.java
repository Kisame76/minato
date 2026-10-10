// The written part of VoiceInteractor (the Unity test APK retest 10: Activity's methods name the type, and a class whose method
// types cannot be resolved cannot be read by reflection), which tools/managergen joins with the API's methods. Minato
// has no voice interaction service (LocalActivityTaskManager.supportsLocalVoiceInteraction answers false), so no
// activity is ever given an interactor; the hidden members Activity calls are here for the compile and keep nothing.
import android.content.Context;
import android.os.Looper;
import com.android.internal.app.IVoiceInteractor;
import java.io.FileDescriptor;
import java.io.PrintWriter;

class VoiceInteractor {
    /** @hide */
    VoiceInteractor(IVoiceInteractor interactor, Context context, Activity activity, Looper looper) {
    }

    /** @hide */
    void attachActivity(Activity activity) {
    }

    /** @hide */
    void retainInstance() {
    }

    /** @hide */
    void detachActivity() {
    }

    /** @hide */
    void dump(String prefix, FileDescriptor fd, PrintWriter writer, String[] args) {
    }

    public static class Request {
        /** @hide */
        void clear() {
        }
    }
}
