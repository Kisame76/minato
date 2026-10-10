package io.github.kisame76.minato.test.accounts;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.AccountManagerCallback;
import android.accounts.AccountManagerFuture;
import android.accounts.AuthenticatorException;
import android.accounts.OnAccountsUpdateListener;
import android.accounts.OperationCanceledException;
import android.app.Application;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Parcel;
import android.util.Log;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The test of android.accounts, without a window (retest 8's resolve list, batch B2): AccountManager as an app sees it
 * that holds no account permission and has no account, which is what Minato is. It logs under the tag accounts-test
 * what each call answers, or the exception it throws; a user id in a message is masked, as it differs by device. The
 * calls run on a worker thread, as a future's getResult refuses the main thread. What differs by device (the
 * authenticator types) is only shown, under accounts-test-device. tools/android-reference.sh takes the log from
 * Android 15, and tools/accounts-check.sh compares Minato's with it.
 */
public class AccountsApplication extends Application {
    private static final String TAG = "accounts-test";
    private static final String TYPE = "io.github.kisame76.minato.test";

    @Override
    public void onCreate() {
        super.onCreate();
        new Thread(this::run, "accounts-test").start();
    }

    private void run() {
        try {
            AccountManager am = AccountManager.get(this);
            // The callbacks' thread: the main thread is busy with the activity's start while the test runs.
            HandlerThread callbacks = new HandlerThread("accounts-callbacks");
            callbacks.start();
            Account account = new Account("minato", TYPE);
            call("get is the same", () -> AccountManager.get(this) == am);
            call("getAccounts", () -> am.getAccounts().length);
            call("getAccountsByType null", () -> am.getAccountsByType(null).length);
            call("getAccountsByType", () -> am.getAccountsByType(TYPE).length);
            call("getAccountsByTypeAndFeatures", () -> {
                AccountManagerFuture<Account[]> f = am.getAccountsByTypeAndFeatures(TYPE, new String[] {"f"}, null, null);
                return f.getResult().length + " done " + f.isDone();
            });
            call("getAccountsByTypeAndFeatures callback", () -> {
                CountDownLatch latch = new CountDownLatch(1);
                AccountManagerCallback<Account[]> callback = future -> latch.countDown();
                am.getAccountsByTypeAndFeatures(TYPE, null, callback, new Handler(callbacks.getLooper()));
                return latch.await(5, TimeUnit.SECONDS);
            });
            Log.i(TAG + "-device", "authenticator types " + am.getAuthenticatorTypes().length);
            call("getPassword", () -> am.getPassword(account));
            call("getUserData", () -> am.getUserData(account, "k"));
            call("peekAuthToken", () -> am.peekAuthToken(account, "t"));
            call("getPreviousName", () -> am.getPreviousName(account));
            call("setUserData", () -> {
                am.setUserData(account, "k", "v");
                return "returned";
            });
            call("setPassword", () -> {
                am.setPassword(account, "pw");
                return "returned";
            });
            call("clearPassword", () -> {
                am.clearPassword(account);
                return "returned";
            });
            call("setAuthToken", () -> {
                am.setAuthToken(account, "t", "tok");
                return "returned";
            });
            call("invalidateAuthToken", () -> {
                am.invalidateAuthToken(TYPE, "tok");
                return "returned";
            });
            call("addAccountExplicitly", () -> am.addAccountExplicitly(account, "pw", null));
            call("removeAccountExplicitly", () -> am.removeAccountExplicitly(account));
            call("notifyAccountAuthenticated", () -> am.notifyAccountAuthenticated(account));
            call("getAccountVisibility", () -> am.getAccountVisibility(account, "com.example"));
            call("setAccountVisibility", () -> am.setAccountVisibility(account, "com.example",
                    AccountManager.VISIBILITY_VISIBLE));
            call("getPackagesAndVisibilityForAccount", () -> am.getPackagesAndVisibilityForAccount(account).size());
            call("getAccountsAndVisibilityForPackage", () -> am.getAccountsAndVisibilityForPackage(
                    getPackageName(), TYPE).size());
            call("renameAccount", () -> am.renameAccount(account, "other", null, null).getResult());
            call("hasFeatures", () -> am.hasFeatures(account, new String[] {"f"}, null, null).getResult());
            call("addAccount", () -> am.addAccount(TYPE, "t", null, null, null, null, null).getResult());
            call("getAuthToken", () -> am.getAuthToken(account, "t", null, false, null, null).getResult());
            call("removeAccount", () -> am.removeAccount(account, null, null).getResult());
            call("updateCredentials", () -> am.updateCredentials(account, "t", null, null, null, null).getResult());
            call("confirmCredentials", () -> am.confirmCredentials(account, null, null, null, null).getResult());
            call("editProperties", () -> am.editProperties(TYPE, null, null, null).getResult());
            call("blockingGetAuthToken", () -> am.blockingGetAuthToken(account, "t", false));
            call("listener", () -> {
                Account[][] seen = new Account[1][];
                CountDownLatch latch = new CountDownLatch(1);
                OnAccountsUpdateListener listener = accounts -> {
                    seen[0] = accounts;
                    latch.countDown();
                };
                am.addOnAccountsUpdatedListener(listener, new Handler(callbacks.getLooper()), true);
                boolean called = latch.await(5, TimeUnit.SECONDS);
                am.removeOnAccountsUpdatedListener(listener);
                return called + " " + (seen[0] == null ? "none" : String.valueOf(seen[0].length));
            });
            call("listener twice", () -> {
                OnAccountsUpdateListener listener = accounts -> { };
                am.addOnAccountsUpdatedListener(listener, null, false);
                try {
                    am.addOnAccountsUpdatedListener(listener, null, false);
                    return "added twice";
                } finally {
                    am.removeOnAccountsUpdatedListener(listener);
                }
            });
            call("newChooseAccountIntent", () -> {
                Intent intent = AccountManager.newChooseAccountIntent(null, null, new String[] {TYPE}, null, null,
                        null, null);
                List<String> keys = new ArrayList<>(intent.getExtras().keySet());
                Collections.sort(keys);
                return intent.getComponent() + " " + keys;
            });
            call("Account", () -> {
                Account same = new Account("minato", TYPE);
                Parcel parcel = Parcel.obtain();
                account.writeToParcel(parcel, 0);
                parcel.setDataPosition(0);
                Account back = Account.CREATOR.createFromParcel(parcel);
                parcel.recycle();
                return account + " " + account.equals(same) + " " + (account.hashCode() == same.hashCode()) + " "
                        + back.equals(account);
            });
            call("Account null name", () -> new Account((String) null, TYPE));
            call("Account empty type", () -> new Account("n", ""));
            call("exceptions", () -> new AuthenticatorException("a") + " " + new OperationCanceledException("o") + " "
                    + new AuthenticatorException(new RuntimeException("c")).getCause());
            call("keys", () -> AccountManager.KEY_ACCOUNT_NAME + " " + AccountManager.KEY_ACCOUNT_TYPE + " "
                    + AccountManager.KEY_AUTHTOKEN + " " + AccountManager.KEY_INTENT + " "
                    + AccountManager.KEY_ERROR_CODE + " " + AccountManager.ERROR_CODE_BAD_ARGUMENTS + " "
                    + AccountManager.VISIBILITY_NOT_VISIBLE + " " + AccountManager.LOGIN_ACCOUNTS_CHANGED_ACTION);
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    private static void call(String name, Callable<?> call) {
        String answer;
        try {
            Object result = call.call();
            answer = result instanceof Object[] ? Arrays.toString((Object[]) result) : String.valueOf(result);
            if (result instanceof Bundle) {
                answer = "bundle " + ((Bundle) result).keySet();
            }
        } catch (Throwable t) {
            answer = t.getClass().getName() + ": " + t.getMessage();
        }
        Log.i(TAG, name + ": " + answer.replaceAll("uid[ =]\\d+", "uid N").replaceAll("pid \\d+", "pid N"));
    }
}
