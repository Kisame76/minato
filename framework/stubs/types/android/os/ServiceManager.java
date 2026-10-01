package android.os;

/**
 * Compile-only stub (ADR-0039 b3.1), never in framework.jar: Build.getSerial asks it; ADR-0045 decision 2's table
 * replaces it.
 */
public class ServiceManager {
    public static IBinder getService(String name) {
        throw new RuntimeException("stub");
    }
}
