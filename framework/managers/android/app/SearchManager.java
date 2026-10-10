// The written part of SearchManager (the Unity test APK retest 10: Activity's methods name the type, and a class whose method types
// cannot be resolved cannot be read by reflection), which tools/managergen joins with the API's methods. AOSP's class
// talks to the system's search service and shows the search dialog; Minato has neither yet (getSystemService returns
// null for "search"), so the class is the API with the hidden members the taken files call: the constructor, and
// PhoneWindow's assist key, which does nothing here.
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.ServiceManager.ServiceNotFoundException;

class SearchManager {
    /** @hide */
    SearchManager(Context context, Handler handler) throws ServiceNotFoundException {
    }

    /** @hide */
    public void launchAssist(Bundle args) {
    }
}
