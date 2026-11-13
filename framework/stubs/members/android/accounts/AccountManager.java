// Hidden members of the public API's compile-only stub (ADR-0045), which tools/managergen joins with
// it: UserManager.createProfileForUser's shared accounts, a system API that needs MANAGE_USERS or CREATE_USERS.
class AccountManager {
    /** @hide */
    public void addSharedAccountsFromParentUser(android.os.UserHandle parentUser, android.os.UserHandle user) {
        throw new RuntimeException("stub");
    }
}
