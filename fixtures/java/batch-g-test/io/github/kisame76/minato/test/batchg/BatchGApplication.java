package io.github.kisame76.minato.test.batchg;

import android.app.Application;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.Array;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.Callable;

/**
 * Two groups of classes that apps and libraries name (batch G): the keystore's StrongBoxUnavailableException,
 * KeyStoreException and KeyProtection, and BackupManager with BackupAgentHelper and its helpers. Each is logged with its
 * result, or with the class of the exception it throws. The classes are reached by name where Minato may lack them,
 * so that the same sources build against Android's SDK and against Minato's framework.
 */
public class BatchGApplication extends Application {
    private static final String TAG = "batch-g-test";
    private final Handler mHandler = new Handler(Looper.getMainLooper());

    private static void log(String name, Callable<Object> call) {
        log(TAG, name, call);
    }

    private static void log(String tag, String name, Callable<Object> call) {
        String result;
        try {
            final Object value = call.call();
            result = text(value);
        } catch (InvocationTargetException e) {
            result = "throws " + e.getCause().getClass().getName();
        } catch (Throwable e) {
            result = "throws " + e.getClass().getName();
        }
        Log.i(tag, name + ": " + result);
    }

    private static String text(Object value) {
        if (value == null) return "null";
        if (value.getClass().isArray()) {
            final List<String> parts = new ArrayList<>();
            for (int i = 0; i < Array.getLength(value); i++) parts.add(text(Array.get(value, i)));
            return parts.toString();
        }
        return value instanceof Collection ? "size " + ((Collection<?>) value).size() : String.valueOf(value);
    }

    /** Every public method without arguments that the class itself declares, by name, with its result. */
    private static void dump(String label, Object target, String... only) {
        final List<String> wanted = Arrays.asList(only);
        final List<Method> methods = new ArrayList<>(Arrays.asList(target.getClass().getDeclaredMethods()));
        Collections.sort(methods, (a, b) -> a.getName().compareTo(b.getName()));
        for (Method method : methods) {
            if (!Modifier.isPublic(method.getModifiers()) || method.getParameterTypes().length != 0
                    || method.isSynthetic() || Modifier.isStatic(method.getModifiers())) {
                continue;
            }
            final String name = method.getName();
            if (!wanted.contains(name)) continue;
            log(label + "." + name + "()", () -> method.invoke(target));
        }
    }

    /** The public static final int and String fields of a class, by name, as the constants an app compiles in. */
    private static void constants(String type, String... only) {
        final List<String> wanted = Arrays.asList(only);
        final StringBuilder out = new StringBuilder();
        try {
            final java.lang.reflect.Field[] fields = Class.forName(type).getDeclaredFields();
            Arrays.sort(fields, (a, b) -> a.getName().compareTo(b.getName()));
            for (java.lang.reflect.Field field : fields) {
                final int mods = field.getModifiers();
                if (wanted.contains(field.getName()) && Modifier.isPublic(mods) && Modifier.isStatic(mods)
                        && Modifier.isFinal(mods) && (field.getType() == int.class || field.getType() == String.class)) {
                    out.append(field.getName()).append('=').append(field.get(null)).append(' ');
                }
            }
        } catch (Throwable e) {
            out.append("throws ").append(e.getClass().getName());
        }
        Log.i(TAG, type.substring(type.lastIndexOf('.') + 1) + " constants: " + out.toString().trim());
    }

    private static boolean accepts(Method candidate, Object[] args) {
        final Class<?>[] types = candidate.getParameterTypes();
        if (types.length != args.length) return false;
        for (int i = 0; i < types.length; i++) {
            if (args[i] == null) {
                if (types[i].isPrimitive()) return false;
            } else if (types[i].isPrimitive()) {
                if (types[i] == int.class && !(args[i] instanceof Integer)) return false;
                if (types[i] == long.class && !(args[i] instanceof Long)) return false;
                if (types[i] == boolean.class && !(args[i] instanceof Boolean)) return false;
            } else if (!types[i].isInstance(args[i])) {
                return false;
            }
        }
        return true;
    }

    private static Object call(Object target, String method, Object... args) throws Exception {
        for (Method candidate : target.getClass().getMethods()) {
            if (candidate.getName().equals(method) && accepts(candidate, args)) {
                return candidate.invoke(target, args);
            }
        }
        throw new NoSuchMethodException(target.getClass().getName() + "." + method);
    }

    private static Object callStatic(String type, String method, Object... args) throws Exception {
        for (Method candidate : Class.forName(type).getMethods()) {
            if (candidate.getName().equals(method) && accepts(candidate, args)) {
                return candidate.invoke(null, args);
            }
        }
        throw new NoSuchMethodException(type + "." + method);
    }

    private static Object construct(String type, Object... args) throws Exception {
        for (Constructor<?> candidate : Class.forName(type).getConstructors()) {
            final Class<?>[] types = candidate.getParameterTypes();
            boolean fits = types.length == args.length;
            for (int i = 0; fits && i < types.length; i++) {
                fits = args[i] == null ? !types[i].isPrimitive()
                        : types[i].isPrimitive() || types[i].isInstance(args[i]);
            }
            if (fits) return candidate.newInstance(args);
        }
        throw new NoSuchMethodException(type + ".<init>");
    }

    @Override
    public void onCreate() {
        super.onCreate();
        mHandler.post(this::run);
    }

    private void run() {
        keystore();
        backup();
        Log.i(TAG, "done");
    }

    private void keystore() {
        final String strong = "android.security.keystore.StrongBoxUnavailableException";
        log("StrongBoxUnavailableException()", () -> {
            final Object e = construct(strong);
            return e.getClass().getName() + " message " + ((Throwable) e).getMessage() + " provider "
                    + (e instanceof java.security.ProviderException);
        });
        log("StrongBoxUnavailableException(msg, cause)", () -> {
            final Throwable cause = new RuntimeException("c");
            final Throwable e = (Throwable) construct(strong, "m", cause);
            return e.getMessage() + " cause " + (e.getCause() == cause);
        });
        log("StrongBoxUnavailableException(cause)", () -> {
            final Throwable cause = new RuntimeException("c");
            final Throwable e = (Throwable) construct(strong, cause);
            return e.getMessage() + " cause " + (e.getCause() == cause);
        });
        final String type = "android.security.KeyStoreException";
        constants(type, "ERROR_ATTESTATION_CHALLENGE_TOO_LARGE", "ERROR_ATTESTATION_KEYS_UNAVAILABLE", "ERROR_ID_ATTESTATION_FAILURE", "ERROR_INCORRECT_USAGE", "ERROR_INTERNAL_SYSTEM_ERROR", "ERROR_KEYMINT_FAILURE", "ERROR_KEYSTORE_FAILURE", "ERROR_KEYSTORE_UNINITIALIZED", "ERROR_KEY_CORRUPTED", "ERROR_KEY_DOES_NOT_EXIST", "ERROR_KEY_NOT_TEMPORALLY_VALID", "ERROR_KEY_OPERATION_EXPIRED", "ERROR_OTHER", "ERROR_PERMISSION_DENIED", "ERROR_UNIMPLEMENTED", "ERROR_USER_AUTHENTICATION_REQUIRED", "RETRY_AFTER_NEXT_REBOOT", "RETRY_NEVER", "RETRY_WHEN_CONNECTIVITY_AVAILABLE", "RETRY_WITH_EXPONENTIAL_BACKOFF");
        log("KeyStoreException superclass", () -> Class.forName(type).getSuperclass().getName());
        log("KeyStoreException is a GeneralSecurityException", () ->
                java.security.GeneralSecurityException.class.isAssignableFrom(Class.forName(type)));
        final String protection = "android.security.keystore.KeyProtection$Builder";
        constants("android.security.keystore.KeyProperties", "AUTH_BIOMETRIC_STRONG", "AUTH_DEVICE_CREDENTIAL", "BLOCK_MODE_CBC", "BLOCK_MODE_CTR", "BLOCK_MODE_ECB", "BLOCK_MODE_GCM", "DIGEST_MD5", "DIGEST_NONE", "DIGEST_SHA1", "DIGEST_SHA224", "DIGEST_SHA256", "DIGEST_SHA384", "DIGEST_SHA512", "ENCRYPTION_PADDING_NONE", "ENCRYPTION_PADDING_PKCS7", "ENCRYPTION_PADDING_RSA_OAEP", "ENCRYPTION_PADDING_RSA_PKCS1", "KEY_ALGORITHM_3DES", "KEY_ALGORITHM_AES", "KEY_ALGORITHM_EC", "KEY_ALGORITHM_HMAC_SHA1", "KEY_ALGORITHM_HMAC_SHA224", "KEY_ALGORITHM_HMAC_SHA256", "KEY_ALGORITHM_HMAC_SHA384", "KEY_ALGORITHM_HMAC_SHA512", "KEY_ALGORITHM_RSA", "NAMESPACE_APPLICATION", "NAMESPACE_WIFI", "ORIGIN_GENERATED", "ORIGIN_IMPORTED", "ORIGIN_SECURELY_IMPORTED", "ORIGIN_UNKNOWN", "PURPOSE_AGREE_KEY", "PURPOSE_ATTEST_KEY", "PURPOSE_DECRYPT", "PURPOSE_ENCRYPT", "PURPOSE_SIGN", "PURPOSE_VERIFY", "PURPOSE_WRAP_KEY", "SECURITY_LEVEL_SOFTWARE", "SECURITY_LEVEL_STRONGBOX", "SECURITY_LEVEL_TRUSTED_ENVIRONMENT", "SECURITY_LEVEL_UNKNOWN", "SECURITY_LEVEL_UNKNOWN_SECURE", "SIGNATURE_PADDING_RSA_PKCS1", "SIGNATURE_PADDING_RSA_PSS", "UNRESTRICTED_USAGE_COUNT");
        log("KeyProtection.Builder(ENCRYPT|DECRYPT).build()", () -> {
            final Object builder = construct(protection, 3);
            final Object built = call(builder, "build");
            dump("KeyProtection", built, "getBlockModes", "getDigests", "getEncryptionPaddings", "getKeyValidityForConsumptionEnd", "getKeyValidityForOriginationEnd", "getKeyValidityStart", "getMaxUsageCount", "getMgf1Digests", "getPurposes", "getSignaturePaddings", "getUserAuthenticationType", "getUserAuthenticationValidityDurationSeconds", "isDigestsSpecified", "isInvalidatedByBiometricEnrollment", "isMgf1DigestsSpecified", "isRandomizedEncryptionRequired", "isUnlockedDeviceRequired", "isUserAuthenticationRequired", "isUserAuthenticationValidWhileOnBody", "isUserConfirmationRequired", "isUserPresenceRequired");
            return built.getClass().getName();
        });
        log("KeyProtection.Builder full", () -> {
            final Object builder = construct(protection, 3 | 4 | 8);
            call(builder, "setBlockModes", (Object) new String[] {"GCM", "CBC"});
            call(builder, "setEncryptionPaddings", (Object) new String[] {"NoPadding", "PKCS7Padding"});
            call(builder, "setSignaturePaddings", (Object) new String[] {"PKCS1"});
            call(builder, "setDigests", (Object) new String[] {"SHA-256", "SHA-512"});
            call(builder, "setRandomizedEncryptionRequired", false);
            call(builder, "setUserAuthenticationRequired", true);
            call(builder, "setUserAuthenticationValidityDurationSeconds", 30);
            call(builder, "setUserConfirmationRequired", false);
            call(builder, "setUnlockedDeviceRequired", false);
            call(builder, "setIsStrongBoxBacked", false);
            call(builder, "setInvalidatedByBiometricEnrollment", false);
            call(builder, "setMaxUsageCount", 5);
            final Object built = call(builder, "build");
            dump("KeyProtection full", built, "getBlockModes", "getDigests", "getEncryptionPaddings", "getKeyValidityForConsumptionEnd", "getKeyValidityForOriginationEnd", "getKeyValidityStart", "getMaxUsageCount", "getMgf1Digests", "getPurposes", "getSignaturePaddings", "getUserAuthenticationType", "getUserAuthenticationValidityDurationSeconds", "isDigestsSpecified", "isInvalidatedByBiometricEnrollment", "isMgf1DigestsSpecified", "isRandomizedEncryptionRequired", "isUnlockedDeviceRequired", "isUserAuthenticationRequired", "isUserAuthenticationValidWhileOnBody", "isUserConfirmationRequired", "isUserPresenceRequired");
            return "built";
        });
        log("KeyProtection.Builder bad purposes", () -> {
            final Object builder = construct(protection, 3);
            call(builder, "setBlockModes", (Object) null);
            return "set";
        });
        log("KeyProtection.Builder validity end before start", () -> {
            final Object builder = construct(protection, 3);
            call(builder, "setKeyValidityStart", new java.util.Date(1000));
            call(builder, "setKeyValidityEnd", new java.util.Date(5000));
            final Object built = call(builder, "build");
            return call(built, "getKeyValidityStart") + " " + call(built, "getKeyValidityForOriginationEnd");
        });
    }

    private void backup() {
        final String manager = "android.app.backup.BackupManager";
        constants(manager, "ERROR_AGENT_FAILURE", "ERROR_BACKUP_CANCELLED", "ERROR_BACKUP_NOT_ALLOWED", "ERROR_PACKAGE_NOT_FOUND", "ERROR_TRANSPORT_ABORTED", "ERROR_TRANSPORT_INVALID", "ERROR_TRANSPORT_PACKAGE_REJECTED", "ERROR_TRANSPORT_QUOTA_EXCEEDED", "ERROR_TRANSPORT_UNAVAILABLE", "FLAG_NON_INCREMENTAL_BACKUP", "PACKAGE_MANAGER_SENTINEL", "SUCCESS");
        final Object backup = construct_(manager, this);
        log("BackupManager.dataChanged()", () -> call(backup, "dataChanged"));
        log("BackupManager.dataChanged(pkg)", () -> callStatic(manager, "dataChanged", getPackageName()));
        log("BackupManager.dataChanged(unknown)", () -> callStatic(manager, "dataChanged", "no.such.app"));
        log("BackupManager.requestRestore(null)", () -> call(backup, "requestRestore", (Object) null));
        log("BackupManager.isBackupEnabled()", () -> call(backup, "isBackupEnabled"));
        final String agent = "android.app.backup.BackupAgent";
        constants(agent, "FLAG_CLIENT_SIDE_ENCRYPTION_ENABLED", "FLAG_DEVICE_TO_DEVICE_TRANSFER", "TYPE_DIRECTORY", "TYPE_FILE");
        constants("android.app.backup.BackupAgentHelper");
        constants("android.app.backup.BackupDataOutput");
        log("TestBackupAgent is a BackupAgentHelper", () -> {
            final Object a = new TestBackupAgent();
            return (a instanceof android.app.backup.BackupAgentHelper) + " "
                    + (a instanceof android.app.backup.BackupAgent) + " " + (a instanceof android.content.ContextWrapper);
        });
        log("SharedPreferencesBackupHelper(ctx, name)", () -> {
            final Object helper = construct("android.app.backup.SharedPreferencesBackupHelper", this,
                    new String[] {"batch-g-prefs"});
            return helper.getClass().getName() + " " + (helper instanceof android.app.backup.BackupHelper);
        });
        log("FileBackupHelper(ctx, files)", () -> {
            final Object helper = construct("android.app.backup.FileBackupHelper", this,
                    new String[] {"batch-g.txt"});
            return helper.getClass().getName() + " " + (helper instanceof android.app.backup.BackupHelper);
        });
        log("manifest backup agent", () -> getApplicationInfo().backupAgentName);
        log("manifest allowBackup", () -> (getApplicationInfo().flags
                & android.content.pm.ApplicationInfo.FLAG_ALLOW_BACKUP) != 0);
    }

    private Object construct_(String type, Object... args) {
        try {
            return construct(type, args);
        } catch (Throwable e) {
            Log.i(TAG, type + " construct: throws " + e.getClass().getName());
            return null;
        }
    }
}
