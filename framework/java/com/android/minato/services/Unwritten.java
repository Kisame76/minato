package com.android.minato.services;

import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * The log line of a method that the managers' generator wrote and no one has written yet (ADR-0045 decision 3):
 * once per method, "Unwritten: <class>.<method>(<parameters>) is not implemented[, returns <default>]". Plain Java,
 * so tools/framework-host-check.sh runs it on the host's JVM; the lines go to stderr until the framework sets its
 * logger.
 */
public final class Unwritten {
    private static final Set<String> logged = ConcurrentHashMap.newKeySet();
    private static volatile ServiceStub.Logger logger = System.err::println;

    private Unwritten() {
    }

    public static void setLogger(ServiceStub.Logger to) {
        logger = to;
    }

    /**
     * @param method  the class, the method's name and its parameter types as the API names them
     * @param returns the default the method returns, as text, or null for void
     */
    public static void log(String method, String returns) {
        if (logged.add(method)) {
            String value = returns == null ? "" : ", returns " + returns;
            logger.log("Unwritten: " + method + " is not implemented" + value);
        }
    }

    /**
     * A placeholder's ctor or method (tools/managergen --placeholder): a class that only has to load, since a class
     * apps reflect over names it, and that Minato does not offer. Logs once, "Unwritten: <method> is a placeholder,
     * throws UnsupportedOperationException", and gives the exception the body throws.
     *
     * @param method the class, the method's name and its parameter types as the API names them
     */
    public static UnsupportedOperationException placeholder(String method) {
        if (logged.add(method)) {
            logger.log("Unwritten: " + method + " is a placeholder, throws UnsupportedOperationException");
        }
        return new UnsupportedOperationException(method + " is not available on this device");
    }

    /** For the host tests: every method logs again. */
    public static void forgetLogged() {
        logged.clear();
    }
}
