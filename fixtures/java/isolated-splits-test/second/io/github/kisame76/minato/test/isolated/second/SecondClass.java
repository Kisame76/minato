package io.github.kisame76.minato.test.isolated.second;

import io.github.kisame76.minato.test.isolated.feature.FeatureClass;

/** A class of the second feature split, which extends one of the first, as a split that uses another may. */
public class SecondClass extends FeatureClass {
    public static String second() {
        return "the second split answers, over " + answer();
    }
}
