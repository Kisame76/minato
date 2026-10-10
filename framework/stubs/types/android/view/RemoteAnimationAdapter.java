package android.view;

/**
 * Compile-only stub (App, the Unity test APK retest 10), never in framework.jar: RemoteAnimationDefinition, the type of Activity's
 * registerRemoteAnimations, holds the system's remote animations, which no app gets without the system's permission.
 */
public class RemoteAnimationAdapter implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<RemoteAnimationAdapter> CREATOR = null;

    public IRemoteAnimationRunner getRunner() { throw new RuntimeException("stub"); }
    public void setCallingPidUid(int pid, int uid) { throw new RuntimeException("stub"); }
    public int describeContents() { throw new RuntimeException("stub"); }
    public void writeToParcel(android.os.Parcel dest, int flags) { throw new RuntimeException("stub"); }
}
