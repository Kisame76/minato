// The written part of MediaSessionManager (services batch B): the manager an app asks for its media controls. An app
// with no notification listener and no MEDIA_CONTENT_CONTROL cannot list or watch the sessions, which Android 15
// answers with a SecurityException, the emulator's (apk-runner-35) answer [verified: services-b-test]. An app's own
// sessions (MediaSession) are made by the local session service (batch G).
import android.content.ComponentName;
import android.content.Context;

class MediaSessionManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public MediaSessionManager(Context context) {
        mContext = context;
    }

    /**
     * @hide MediaSession's: the session service makes the session ("media_session", ours: LocalMediaSessionService).
     */
    public ISession createSession(ISessionCallback cbStub, String tag, android.os.Bundle sessionInfo)
            throws android.os.RemoteException {
        return ISessionManager.Stub.asInterface(android.os.ServiceManager.getService(Context.MEDIA_SESSION_SERVICE))
                .createSession(mContext.getPackageName(), cbStub, tag, sessionInfo, android.os.UserHandle.myUserId());
    }

    public java.util.List<MediaController> getActiveSessions(ComponentName notificationListener) {
        throw new SecurityException("Missing permission to control media.");
    }

    public void addOnActiveSessionsChangedListener(OnActiveSessionsChangedListener sessionListener,
            ComponentName notificationListener) {
        throw new SecurityException("Missing permission to control media.");
    }
}
