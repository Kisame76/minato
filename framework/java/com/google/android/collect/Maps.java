/*
 * Ours (retest 8's batch B2): com.google.android.collect.Maps under AOSP's name, whose folder of frameworks/base is not
 * fetched. AccountManager's constructor makes its two maps of auth tokens with it. Android 15's has these two methods
 * [verified: frameworks/base at android-15.0.0_r1].
 */
package com.google.android.collect;

import android.util.ArrayMap;

import java.util.HashMap;

/** @hide */
public class Maps {
    /** A new, empty HashMap. */
    public static <K, V> HashMap<K, V> newHashMap() {
        return new HashMap<K, V>();
    }

    /** A new, empty ArrayMap. */
    public static <K, V> ArrayMap<K, V> newArrayMap() {
        return new ArrayMap<K, V>();
    }
}
