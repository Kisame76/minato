package android.os;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: UserManager.createUser's request, a system
 * API that needs MANAGE_USERS or CREATE_USERS, which no app holds.
 */
public final class NewUserRequest {
    public String getName() {
        throw new RuntimeException("stub");
    }

    public String getUserType() {
        throw new RuntimeException("stub");
    }

    int getFlags() {
        throw new RuntimeException("stub");
    }

    public android.graphics.Bitmap getUserIcon() {
        throw new RuntimeException("stub");
    }

    public String getAccountName() {
        throw new RuntimeException("stub");
    }

    public String getAccountType() {
        throw new RuntimeException("stub");
    }

    public PersistableBundle getAccountOptions() {
        throw new RuntimeException("stub");
    }
}
