package android.os;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 1b), never in framework.jar: UserManager.createUser's answer, a system
 * API that needs MANAGE_USERS or CREATE_USERS, which no app holds.
 */
public final class NewUserResponse {
    public NewUserResponse(UserHandle user, int operationResult) {
        throw new RuntimeException("stub");
    }
}
