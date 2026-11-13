package android.os;

/**
 * Compile-only stub (the battery), never in framework.jar: the battery stats' name,
 * which BatteryManager's registry entry and ServiceManager's table take; javac puts the constant in, so nothing loads
 * this class. Minato keeps no battery history.
 */
public abstract class BatteryStats {
    public static final String SERVICE_NAME = "batterystats";
}
