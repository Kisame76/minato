/*
 * Ours (widgets): com.google.android.collect.Lists under AOSP's name, whose
 * folder of frameworks/base is not fetched. ListView's constructor makes its header and footer lists with it, and
 * View's debug dump names it. Android 15's has these two methods [verified: frameworks/base at android-15.0.0_r1].
 */
package com.google.android.collect;

import java.util.ArrayList;

/** @hide */
public class Lists {
    /** A new, empty ArrayList. */
    public static <E> ArrayList<E> newArrayList() {
        return new ArrayList<>();
    }

    /** A new ArrayList of the elements, in their order. */
    @SafeVarargs
    public static <E> ArrayList<E> newArrayList(E... elements) {
        ArrayList<E> list = new ArrayList<>(elements.length);
        for (E element : elements) {
            list.add(element);
        }
        return list;
    }
}
