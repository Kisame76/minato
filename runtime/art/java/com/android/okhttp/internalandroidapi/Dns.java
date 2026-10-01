/*
 * Minato: a compile-time stand-in (ADR-0033 a) for external/okhttp's com.android.okhttp.internalandroidapi.Dns, which
 * libcore's libcore.net.http.Dns extends. Soong compiles libcore against okhttp's sources (core-all); the boot jars do
 * not contain okhttp, and neither do ours (runtime/art/boot_jars.py). Its classes come with okhttp's own jar once an
 * app needs it (ADR-0033 decision 10).
 */
package com.android.okhttp.internalandroidapi;

import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.List;

public interface Dns {
    List<InetAddress> lookup(String hostname) throws UnknownHostException;
}
