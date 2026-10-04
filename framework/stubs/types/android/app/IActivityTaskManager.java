package android.app;

/**
 * Compile-only stub (ADR-0039 step c2c3), never in framework.jar: ActivityTaskManager.getService's type, which Activity
 * asks; ADR-0045's local activity task service answers it.
 */
public interface IActivityTaskManager extends android.os.IInterface {

    int startActivity(android.app.IApplicationThread caller, String callingPackage, String callingFeatureId,
            android.content.Intent intent, String resolvedType, android.os.IBinder resultTo, String resultWho,
            int requestCode, int flags, android.app.ProfilerInfo profilerInfo, android.os.Bundle options)
            throws android.os.RemoteException;

    boolean startNextMatchingActivity(android.os.IBinder callingActivity, android.content.Intent intent,
            android.os.Bundle options) throws android.os.RemoteException;

    int startActivityIntentSender(android.app.IApplicationThread caller, android.content.IIntentSender target,
            android.os.IBinder whitelistToken, android.content.Intent fillInIntent, String resolvedType,
            android.os.IBinder resultTo, String resultWho, int requestCode, int flagsMask, int flagsValues,
            android.os.Bundle options) throws android.os.RemoteException;

    boolean supportsLocalVoiceInteraction() throws android.os.RemoteException;


    int startActivities(android.app.IApplicationThread caller, String callingPackage, String callingFeatureId,
            android.content.Intent[] intents, String[] resolvedTypes, android.os.IBinder resultTo,
            android.os.Bundle options, int userId) throws android.os.RemoteException;

    int startActivityAsUser(android.app.IApplicationThread caller, String callingPackage, String callingFeatureId,
            android.content.Intent intent, String resolvedType, android.os.IBinder resultTo, String resultWho,
            int requestCode, int flags, android.app.ProfilerInfo profilerInfo, android.os.Bundle options, int userId)
            throws android.os.RemoteException;

    int startActivityAsCaller(android.app.IApplicationThread caller, String callingPackage,
            android.content.Intent intent, String resolvedType, android.os.IBinder resultTo, String resultWho,
            int requestCode, int flags, android.app.ProfilerInfo profilerInfo, android.os.Bundle options,
            boolean ignoreTargetSecurity, int userId) throws android.os.RemoteException;

    android.app.IActivityClientController getActivityClientController() throws android.os.RemoteException;
}
