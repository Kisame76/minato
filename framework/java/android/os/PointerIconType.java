/*
 * Ours in place of the class aidl --lang=java writes from frameworks/native's
 * libs/input/android/os/PointerIconType.aidl at android-15.0.0_r1 (ADR-0042 b, gap 6), which PointerIcon's type
 * constants name: frameworks/native's input library is not fetched beyond what libinput's MotionEvent needs. As the
 * generated one, an @interface of the AIDL enum's int values, under its names, which
 * framework/native/include/android/os/PointerIconType.h has for C++ (ADR-0045 step 7b1). The public values are
 * PointerIcon's TYPE_ constants of Android 15's API (api/current.txt); pointer-insets-test checks the hidden ones,
 * CUSTOM, NOT_SPECIFIED and the SPOT_ types, by the names PointerIcon gives them on Android 15.
 */
package android.os;

/** @hide */
public @interface PointerIconType {
    int CUSTOM = -1;
    int TYPE_NULL = 0;
    int NOT_SPECIFIED = 1;
    int ARROW = 1000;
    int CONTEXT_MENU = 1001;
    int HAND = 1002;
    int HELP = 1003;
    int WAIT = 1004;
    int CELL = 1006;
    int CROSSHAIR = 1007;
    int TEXT = 1008;
    int VERTICAL_TEXT = 1009;
    int ALIAS = 1010;
    int COPY = 1011;
    int NO_DROP = 1012;
    int ALL_SCROLL = 1013;
    int HORIZONTAL_DOUBLE_ARROW = 1014;
    int VERTICAL_DOUBLE_ARROW = 1015;
    int TOP_RIGHT_DOUBLE_ARROW = 1016;
    int TOP_LEFT_DOUBLE_ARROW = 1017;
    int ZOOM_IN = 1018;
    int ZOOM_OUT = 1019;
    int GRAB = 1020;
    int GRABBING = 1021;
    int HANDWRITING = 1022;
    int SPOT_HOVER = 2000;
    int SPOT_TOUCH = 2001;
    int SPOT_ANCHOR = 2002;
}
