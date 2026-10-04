// Hidden members of the public API's compile-only stub (ADR-0039 step c2c1), which tools/managergen joins with it:
// ContentProvider checks a cross-profile caller's user.
import android.content.pm.UserProperties;

class UserManager {
    /** @hide */
    public UserProperties getUserProperties(UserHandle userHandle) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public UserHandle getProfileParent(UserHandle user) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static boolean isVisibleBackgroundUsersEnabled() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int getMainDisplayIdAssignedToUser() {
        throw new RuntimeException("stub");
    }
}
