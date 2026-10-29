package android.hardware.health.V1_0;

/**
 * Compile-only stub (the battery, gap 1 of the compatibility plan), never in framework.jar: the health HAL's HIDL
 * constants, which BatteryManager's public constants take; javac puts their values into BatteryManager, so nothing
 * loads this class. The values are Android 15's (core/api/current.txt's BatteryManager).
 */
public final class Constants {
    public static final int BATTERY_STATUS_UNKNOWN = 1;
    public static final int BATTERY_STATUS_CHARGING = 2;
    public static final int BATTERY_STATUS_DISCHARGING = 3;
    public static final int BATTERY_STATUS_NOT_CHARGING = 4;
    public static final int BATTERY_STATUS_FULL = 5;
    public static final int BATTERY_HEALTH_UNKNOWN = 1;
    public static final int BATTERY_HEALTH_GOOD = 2;
    public static final int BATTERY_HEALTH_OVERHEAT = 3;
    public static final int BATTERY_HEALTH_DEAD = 4;
    public static final int BATTERY_HEALTH_OVER_VOLTAGE = 5;
    public static final int BATTERY_HEALTH_UNSPECIFIED_FAILURE = 6;
    public static final int BATTERY_HEALTH_COLD = 7;
}
