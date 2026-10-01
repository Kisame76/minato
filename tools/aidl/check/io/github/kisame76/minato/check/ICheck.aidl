package io.github.kisame76.minato.check;

import io.github.kisame76.minato.check.Mode;
import io.github.kisame76.minato.check.Point;

/** The shape of a service interface: a call with in arguments and a result, and a oneway call. */
interface ICheck {
    Point move(in Point from, Mode mode);
    oneway void ping();
}
