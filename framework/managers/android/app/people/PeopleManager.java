// The written part of PeopleManager (service inventory, bundle B): the manager is there; the conversation shortcuts
// it adds belong to the sharing surface Minato does not have [verified: services-h-test].
import android.content.Context;

class PeopleManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public PeopleManager(Context context) {
        mContext = context;
    }
}
