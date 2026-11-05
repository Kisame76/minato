// The written part of RoleManager (services batch B): the roles of a phone, none of which an app of Minato holds. The
// roles a phone offers are available, as Android 15 answers for HOME [verified: services-b-test]; none is held.
import android.content.Context;

class RoleManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public RoleManager(Context context) {
        mContext = context;
    }

    public boolean isRoleAvailable(String roleName) {
        switch (roleName) {
            case ROLE_ASSISTANT:
            case ROLE_BROWSER:
            case ROLE_CALL_REDIRECTION:
            case ROLE_CALL_SCREENING:
            case ROLE_DIALER:
            case ROLE_HOME:
            case ROLE_SMS:
                return true;
            default:
                return false;
        }
    }

    public boolean isRoleHeld(String roleName) {
        return false;
    }
}
