package com.android.internal.util.function.pooled;

import com.android.internal.util.function.TriConsumer;
import java.util.function.BiConsumer;

/** Compile-only stub (the Unity test APK resolve list, batch E), never in framework.jar: the pooled lambdas of PackageInstaller's session callbacks. */
public interface PooledLambda {
    static <A, B> PooledRunnable obtainRunnable(BiConsumer<? super A, ? super B> function, A arg1, B arg2) {
        return null;
    }

    static <A, B, C> PooledRunnable obtainRunnable(TriConsumer<? super A, ? super B, ? super C> function, A arg1,
            B arg2, C arg3) {
        return null;
    }
}
