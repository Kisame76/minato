/*
 * The user service (ADR-0045 decisions 3 and 4): IUserManager of
 * system_server, local in the app's process, which ServiceManager's table gives as "user" and UserManager asks. Minato
 * has Android's one user of a phone, user 0: the system user, the main and only full user, an admin, unlocked, running
 * and in front, without profiles, restrictions or a guest [verified: services-test on apk-runner-35-bare]. The calls
 * that need MANAGE_USERS or a like permission refuse an app, as UserManagerService refuses it.
 * It is AIDL's Default class, as the power service is, so a call it does not answer gives its type's default. Not
 * under com.android.minato, whose classes tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.os;

public final class UserService extends IUserManager.Default {
    private final Binder mBinder = new Binder();

    private UserService() {
        mBinder.attachInterface(this, IUserManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "user": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new UserService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    /** Whether the user is the one user there is. */
    private static boolean ours(int userId) {
        return userId == UserHandle.USER_SYSTEM;
    }

    @Override
    public boolean isUserUnlocked(int userId) {
        return ours(userId);
    }

    @Override
    public boolean isUserUnlockingOrUnlocked(int userId) {
        return ours(userId);
    }

    @Override
    public boolean isUserRunning(int userId) {
        return ours(userId);
    }

    @Override
    public boolean isUserForeground(int userId) {
        return ours(userId);
    }

    @Override
    public boolean isUserVisible(int userId) {
        return ours(userId);
    }

    @Override
    public boolean isQuietModeEnabled(int userId) {
        return false;
    }

    @Override
    public int[] getProfileIds(int userId, boolean enabledOnly) {
        return ours(userId) ? new int[] {userId} : new int[0];
    }

    /** No user is a profile: the type of a full user is the empty string, as UserManagerService gives it. */
    @Override
    public String getProfileType(int userId) {
        return "";
    }

    @Override
    public int getUserSerialNumber(int userId) {
        return ours(userId) ? 0 : -1;
    }

    @Override
    public int getUserHandle(int userSerialNumber) {
        return userSerialNumber == 0 ? UserHandle.USER_SYSTEM : UserHandle.USER_NULL;
    }

    @Override
    public Bundle getUserRestrictions(int userId) {
        return new Bundle();
    }

    @Override
    public boolean hasUserRestriction(String restrictionKey, int userId) {
        return false;
    }

    @Override
    public Bundle getApplicationRestrictions(String packageName) {
        return new Bundle();
    }

    @Override
    public Bundle getApplicationRestrictionsForUser(String packageName, int userId) {
        return new Bundle();
    }

    @Override
    public int getMainUserId() {
        return UserHandle.USER_SYSTEM;
    }

    @Override
    public boolean isAdminUser(int userId) {
        return ours(userId);
    }

    @Override
    public boolean isDemoUser(int userId) {
        return false;
    }

    @Override
    public boolean isUserOfType(int userId, String userType) {
        return ours(userId) && UserManager.USER_TYPE_FULL_SYSTEM.equals(userType);
    }

    @Override
    public boolean isSameProfileGroup(int userId, int otherUserId) {
        return userId == otherUserId;
    }

    @Override
    public boolean isHeadlessSystemUserMode() {
        return false;
    }

    /**
     * The user's name, which UserManagerService gives only to holders of MANAGE_USERS, CREATE_USERS, QUERY_USERS or
     * GET_ACCOUNTS_PRIVILEGED, system permissions that no app of Minato's holds [verified: services-test]; the
     * message is inferred.
     */
    @Override
    public String getUserName() {
        throw new SecurityException("You need MANAGE_USERS, CREATE_USERS, QUERY_USERS, or GET_ACCOUNTS_PRIVILEGED "
                + "permissions to: get user name");
    }

    /** The users, which need MANAGE_USERS, CREATE_USERS or QUERY_USERS as getUserName's name does; inferred too. */
    @Override
    public java.util.List<android.content.pm.UserInfo> getUsers(boolean excludePartial, boolean excludeDying,
            boolean excludePreCreated) {
        throw new SecurityException("You either need MANAGE_USERS, CREATE_USERS, or QUERY_USERS permission to: "
                + "query users");
    }

    /** As UserManagerService for the system user, which it makes at the first boot with no creation time. */
    @Override
    public long getUserCreationTime(int userId) {
        return 0;
    }
}
