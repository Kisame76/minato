package android.media.projection;

import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;

/**
 * Compile-only stub (ADR-0041 a)), never in framework.jar: IDisplayManager's AIDL code names it for virtual displays,
 * which apps on Minato's path do not make.
 */
public interface IMediaProjection extends IInterface {
    abstract class Stub extends Binder implements IMediaProjection {
        public static IMediaProjection asInterface(IBinder binder) {
            throw new RuntimeException("stub");
        }
    }
}
