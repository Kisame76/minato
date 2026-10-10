/*
 * Ours (retest 8's batch B2): com.google.android.collect.Sets under AOSP's name, whose folder of frameworks/base is not
 * fetched. ChooseTypeAndAccountActivity makes its set of account types with it. Android 15's has these six methods
 * [verified: frameworks/base at android-15.0.0_r1]; they make the sets the way their names say.
 */
package com.google.android.collect;

import android.util.ArraySet;

import java.util.HashSet;
import java.util.SortedSet;
import java.util.TreeSet;

/** @hide */
public class Sets {
    /** A new, empty HashSet. */
    public static <K> HashSet<K> newHashSet() {
        return new HashSet<K>();
    }

    /** A new HashSet of the elements. */
    @SafeVarargs
    public static <E> HashSet<E> newHashSet(E... elements) {
        HashSet<E> set = new HashSet<E>(elements.length);
        for (E element : elements) {
            set.add(element);
        }
        return set;
    }

    /** A new, empty TreeSet. */
    public static <E> SortedSet<E> newSortedSet() {
        return new TreeSet<E>();
    }

    /** A new TreeSet of the elements. */
    @SafeVarargs
    public static <E> SortedSet<E> newSortedSet(E... elements) {
        SortedSet<E> set = new TreeSet<E>();
        for (E element : elements) {
            set.add(element);
        }
        return set;
    }

    /** A new, empty ArraySet. */
    public static <E> ArraySet<E> newArraySet() {
        return new ArraySet<E>();
    }

    /** A new ArraySet of the elements. */
    @SafeVarargs
    public static <E> ArraySet<E> newArraySet(E... elements) {
        ArraySet<E> set = new ArraySet<E>(elements.length);
        for (E element : elements) {
            set.add(element);
        }
        return set;
    }
}
