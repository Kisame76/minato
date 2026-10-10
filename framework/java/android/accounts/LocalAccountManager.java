/*
 * The account service (ADR-0045 decision 4's services; retest 8's resolve list, batch B2): IAccountManager of
 * system_server, local in the app's process, which ServiceManager's table gives as "account" and AOSP's AccountManager
 * asks. It answers as Android 15's AccountManagerService does for an app that holds no account permission, on a device
 * with no account and no authenticator, which is what Minato is [verified: AccountManagerService.java at
 * android-15.0.0_r1, accounts-test on apk-runner-35]: every read of the accounts gives an empty array, the calls on an
 * account that the app's uid does not manage throw the service's SecurityException with its words, the calls that need
 * an authenticator's service end with "bind failure", and a feature query ends with an empty list. Departure: an
 * app's own authenticator service is never bound, so no account is ever added and the app's AbstractAccountAuthenticator
 * never runs; Minato has no account database and no accounts settings. The service's answers to a response come on a
 * worker thread, as the oneway call of a Binder delivers them. Every call not named here is AIDL's Default answer. Not
 * under com.android.minato, whose classes tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.accounts;

import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import android.os.UserHandle;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;

public final class LocalAccountManager extends IAccountManager.Default {
    private static final Account[] EMPTY_ACCOUNT_ARRAY = new Account[0];

    private final Binder mBinder = new Binder();
    /** The thread that delivers a response, as a oneway Binder call does. */
    private final Executor mWorker = Executors.newSingleThreadExecutor(runnable -> {
        Thread thread = new Thread(runnable, "AccountManagerWorker");
        thread.setDaemon(true);
        return thread;
    });

    private LocalAccountManager() {
        mBinder.attachInterface(this, IAccountManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "account": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalAccountManager().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    /** The service's isCrossUser: another user's accounts only for the system and a holder of the permission. */
    private static boolean isCrossUser(int callingUid, int userId) {
        return userId != UserHandle.getCallingUserId() && callingUid != Process.SYSTEM_UID;
    }

    /** What the service's response.onResult/onError do: the answer reaches the app's AmsTask on another thread. */
    private void result(IAccountManagerResponse response, Bundle result) {
        mWorker.execute(() -> {
            try {
                response.onResult(result);
            } catch (RemoteException e) {
                // The app's own response in this process: it does not die.
            }
        });
    }

    private void error(IAccountManagerResponse response, int code, String message) {
        mWorker.execute(() -> {
            try {
                response.onError(code, message);
            } catch (RemoteException e) {
                // As above.
            }
        });
    }

    private static SecurityException notManaged(String format, Object... arguments) {
        return new SecurityException(String.format(format, arguments));
    }

    // The reads: no account, so no account is visible.

    @Override
    public Account[] getAccountsAsUser(String accountType, int userId, String opPackageName) {
        int callingUid = Binder.getCallingUid();
        if (userId != UserHandle.getCallingUserId() && callingUid != Process.SYSTEM_UID) {
            throw new SecurityException("User " + UserHandle.getCallingUserId() + " trying to get account for "
                    + userId);
        }
        return EMPTY_ACCOUNT_ARRAY;
    }

    @Override
    public Account[] getAccountsForPackage(String packageName, int uid, String opPackageName) {
        return EMPTY_ACCOUNT_ARRAY;
    }

    @Override
    public Account[] getAccountsByTypeForPackage(String type, String packageName, String opPackageName) {
        return EMPTY_ACCOUNT_ARRAY;
    }

    @Override
    public AuthenticatorDescription[] getAuthenticatorTypes(int userId) {
        int callingUid = Binder.getCallingUid();
        if (isCrossUser(callingUid, userId)) {
            throw new SecurityException(String.format("User %s tying to get authenticator types for %s",
                    UserHandle.getCallingUserId(), userId));
        }
        return new AuthenticatorDescription[0];
    }

    @Override
    public void getAccountsByFeatures(IAccountManagerResponse response, String accountType, String[] features,
            String opPackageName) {
        if (response == null) {
            throw new IllegalArgumentException("response is null");
        }
        if (accountType == null) {
            throw new IllegalArgumentException("accountType is null");
        }
        Bundle result = new Bundle();
        result.putParcelableArray(AccountManager.KEY_ACCOUNTS, EMPTY_ACCOUNT_ARRAY);
        result(response, result);
    }

    @Override
    public String getPreviousName(Account account) {
        Objects.requireNonNull(account, "account cannot be null");
        return null;
    }

    // The calls on an account, which the app's uid manages none of.

    @Override
    public String getPassword(Account account) {
        if (account == null) {
            throw new IllegalArgumentException("account is null");
        }
        throw notManaged("uid %s cannot get secrets for accounts of type: %s", Binder.getCallingUid(), account.type);
    }

    @Override
    public String getUserData(Account account, String key) {
        Objects.requireNonNull(account, "account cannot be null");
        Objects.requireNonNull(key, "key cannot be null");
        throw notManaged("uid %s cannot get user data for accounts of type: %s", Binder.getCallingUid(),
                account.type);
    }

    @Override
    public String peekAuthToken(Account account, String authTokenType) {
        Objects.requireNonNull(account, "account cannot be null");
        Objects.requireNonNull(authTokenType, "authTokenType cannot be null");
        throw notManaged("uid %s cannot peek the authtokens associated with accounts of type: %s",
                Binder.getCallingUid(), account.type);
    }

    @Override
    public void setAuthToken(Account account, String authTokenType, String authToken) {
        Objects.requireNonNull(account, "account cannot be null");
        Objects.requireNonNull(authTokenType, "authTokenType cannot be null");
        throw notManaged("uid %s cannot set auth tokens associated with accounts of type: %s",
                Binder.getCallingUid(), account.type);
    }

    @Override
    public void setPassword(Account account, String password) {
        Objects.requireNonNull(account, "account cannot be null");
        throw notManaged("uid %s cannot set secrets for accounts of type: %s", Binder.getCallingUid(), account.type);
    }

    @Override
    public void clearPassword(Account account) {
        Objects.requireNonNull(account, "account cannot be null");
        throw notManaged("uid %s cannot clear passwords for accounts of type: %s", Binder.getCallingUid(),
                account.type);
    }

    @Override
    public void setUserData(Account account, String key, String value) {
        if (key == null) {
            throw new IllegalArgumentException("key is null");
        }
        if (account == null) {
            throw new IllegalArgumentException("account is null");
        }
        throw notManaged("uid %s cannot set user data for accounts of type: %s", Binder.getCallingUid(),
                account.type);
    }

    @Override
    public void invalidateAuthToken(String accountType, String authToken) {
        Objects.requireNonNull(accountType, "accountType cannot be null");
        Objects.requireNonNull(authToken, "authToken cannot be null");
        // No account holds a token, so there is none to invalidate.
    }

    @Override
    public boolean addAccountExplicitly(Account account, String password, Bundle extras, String opPackageName) {
        return addAccountExplicitlyWithVisibility(account, password, extras, null, opPackageName);
    }

    @Override
    @SuppressWarnings("rawtypes") // The AIDL interface's own raw Map.
    public boolean addAccountExplicitlyWithVisibility(Account account, String password, Bundle extras, Map visibility,
            String opPackageName) {
        Objects.requireNonNull(account, "account cannot be null");
        throw notManaged("uid=%s, package=%s cannot explicitly add accounts of type: %s", Binder.getCallingUid(),
                opPackageName, account.type);
    }

    @Override
    public boolean removeAccountExplicitly(Account account) {
        if (account == null) {
            return false;
        }
        throw notManaged("uid %s cannot explicitly remove accounts of type: %s", Binder.getCallingUid(),
                account.type);
    }

    @Override
    public boolean accountAuthenticated(Account account) {
        Objects.requireNonNull(account, "account cannot be null");
        throw notManaged("uid %s cannot notify authentication for accounts of type: %s", Binder.getCallingUid(),
                account.type);
    }

    @Override
    public int getAccountVisibility(Account account, String packageName) {
        Objects.requireNonNull(account, "account cannot be null");
        Objects.requireNonNull(packageName, "packageName cannot be null");
        throw notManaged("uid %s cannot get secrets for accounts of type: %s", Binder.getCallingUid(), account.type);
    }

    @Override
    public boolean setAccountVisibility(Account account, String packageName, int newVisibility) {
        Objects.requireNonNull(account, "account cannot be null");
        Objects.requireNonNull(packageName, "packageName cannot be null");
        throw notManaged("uid %s cannot get secrets for accounts of type: %s", Binder.getCallingUid(), account.type);
    }

    @Override
    @SuppressWarnings("rawtypes") // The AIDL interface's own raw Map.
    public Map getPackagesAndVisibilityForAccount(Account account) {
        Objects.requireNonNull(account, "account cannot be null");
        throw notManaged("uid %s cannot get secrets for account %s", Binder.getCallingUid(), account);
    }

    @Override
    @SuppressWarnings("rawtypes") // The AIDL interface's own raw Map.
    public Map getAccountsAndVisibilityForPackage(String packageName, String accountType) {
        // No type is managed by the app's uid, and only the system may ask for all types.
        if (Binder.getCallingUid() != Process.SYSTEM_UID || accountType != null) {
            throw new SecurityException("getAccountsAndVisibilityForPackage() called from unauthorized uid "
                    + Binder.getCallingUid() + " with packageName=" + packageName);
        }
        return new LinkedHashMap<>();
    }

    @Override
    public void renameAccount(IAccountManagerResponse response, Account accountToRename, String newName) {
        if (accountToRename == null) {
            throw new IllegalArgumentException("account is null");
        }
        if (newName != null && newName.length() > 200) {
            throw new IllegalArgumentException("account name longer than 200");
        }
        throw notManaged("uid %s cannot rename accounts of type: %s", Binder.getCallingUid(), accountToRename.type);
    }

    @Override
    public void removeAccountAsUser(IAccountManagerResponse response, Account account, boolean expectActivityLaunch,
            int userId) {
        checkArgument(account != null, "account cannot be null");
        checkArgument(response != null, "response cannot be null");
        int callingUid = Binder.getCallingUid();
        if (isCrossUser(callingUid, userId)) {
            throw new SecurityException(String.format("User %s tying remove account for %s",
                    UserHandle.getCallingUserId(), userId));
        }
        throw notManaged("uid %s cannot remove accounts of type: %s", callingUid, account.type);
    }

    @Override
    public void hasFeatures(IAccountManagerResponse response, Account account, String[] features, int userId,
            String opPackageName) {
        checkArgument(account != null, "account cannot be null");
        checkArgument(response != null, "response cannot be null");
        checkArgument(features != null, "features cannot be null");
        int callingUid = Binder.getCallingUid();
        if (userId != UserHandle.getCallingUserId() && callingUid != Process.SYSTEM_UID) {
            throw new SecurityException("User " + UserHandle.getCallingUserId()
                    + " trying to check account features for " + userId);
        }
        // checkReadAccountsPermitted: the app holds no permission and the type's authenticator does not exist.
        throw new SecurityException(String.format("caller uid %s cannot access %s accounts", callingUid,
                account.type));
    }

    @Override
    public void editProperties(IAccountManagerResponse response, String accountType, boolean expectActivityLaunch) {
        if (response == null) {
            throw new IllegalArgumentException("response is null");
        }
        if (accountType == null) {
            throw new IllegalArgumentException("accountType is null");
        }
        throw notManaged("uid %s cannot edit authenticator properites for account type: %s", Binder.getCallingUid(),
                accountType);
    }

    // The calls that need an authenticator's service, which no account type has: the service cannot bind to it.

    @Override
    public void addAccount(IAccountManagerResponse response, String accountType, String authTokenType,
            String[] requiredFeatures, boolean expectActivityLaunch, Bundle options) {
        if (response == null) {
            throw new IllegalArgumentException("response is null");
        }
        if (accountType == null) {
            throw new IllegalArgumentException("accountType is null");
        }
        error(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "bind failure");
    }

    @Override
    public void addAccountAsUser(IAccountManagerResponse response, String accountType, String authTokenType,
            String[] requiredFeatures, boolean expectActivityLaunch, Bundle options, int userId) {
        checkArgument(response != null, "response cannot be null");
        checkArgument(accountType != null, "accountType cannot be null");
        if (isCrossUser(Binder.getCallingUid(), userId)) {
            throw new SecurityException(String.format("User %s trying to add account for %s",
                    UserHandle.getCallingUserId(), userId));
        }
        error(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "bind failure");
    }

    @Override
    public void getAuthToken(IAccountManagerResponse response, Account account, String authTokenType,
            boolean notifyOnAuthFailure, boolean expectActivityLaunch, Bundle options) {
        checkArgument(response != null, "response cannot be null");
        if (account == null) {
            error(response, AccountManager.ERROR_CODE_BAD_ARGUMENTS, "account is null");
            return;
        }
        if (authTokenType == null) {
            error(response, AccountManager.ERROR_CODE_BAD_ARGUMENTS, "authTokenType is null");
            return;
        }
        String callerPackage = options.getString(AccountManager.KEY_ANDROID_PACKAGE_NAME);
        int callerUid = Binder.getCallingUid();
        if (callerPackage == null || !callerPackage.equals(android.app.ActivityThread.currentPackageName())) {
            throw new SecurityException(String.format("Uid %s is attempting to illegally masquerade as package %s!",
                    callerUid, callerPackage));
        }
        error(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "bind failure");
    }

    @Override
    public void updateCredentials(IAccountManagerResponse response, Account account, String authTokenType,
            boolean expectActivityLaunch, Bundle options) {
        if (response == null) {
            throw new IllegalArgumentException("response is null");
        }
        if (account == null) {
            throw new IllegalArgumentException("account is null");
        }
        error(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "bind failure");
    }

    @Override
    public void confirmCredentialsAsUser(IAccountManagerResponse response, Account account, Bundle options,
            boolean expectActivityLaunch, int userId) {
        if (isCrossUser(Binder.getCallingUid(), userId)) {
            throw new SecurityException(String.format("User %s trying to confirm account credentials for %s",
                    UserHandle.getCallingUserId(), userId));
        }
        if (response == null) {
            throw new IllegalArgumentException("response is null");
        }
        if (account == null) {
            throw new IllegalArgumentException("account is null");
        }
        error(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "bind failure");
    }

    private static void checkArgument(boolean expression, String message) {
        if (!expression) {
            throw new IllegalArgumentException(message);
        }
    }
}
