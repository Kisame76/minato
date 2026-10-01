/*
 * Minato: a compile-time stand-in (ADR-0033 a) for the class aconfig generates from external/icu's icu.aconfig. ICU's
 * sources name its flag only in @FlaggedApi, whose constant javac takes in and drops (source retention), so the boot
 * jars refer to no Flags class; ours do not contain it (runtime/art/boot_jars.py).
 */
package com.android.icu;

public final class Flags {
    public static final String FLAG_ICU_V_API = "com.android.icu.icu_v_api";

    private Flags() {}
}
