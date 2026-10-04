package com.android.internal.util;

import java.util.function.IntFunction;
import java.util.function.Supplier;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: content capture stays off on Minato: no Context
 * gives a ContentCaptureManager, as on a device without the service; its event buffer.
 */
public class RingBuffer<T> {
    public RingBuffer(Class<T> c, int capacity) {
        throw new RuntimeException("stub");
    }

    public RingBuffer(Supplier<T> newItem, IntFunction<T[]> newBacking, int capacity) {
        throw new RuntimeException("stub");
    }

    public int size() {
        throw new RuntimeException("stub");
    }

    public boolean isEmpty() {
        throw new RuntimeException("stub");
    }

    public void clear() {
        throw new RuntimeException("stub");
    }

    public void append(T t) {
        throw new RuntimeException("stub");
    }

    public T[] toArray() {
        throw new RuntimeException("stub");
    }
}
