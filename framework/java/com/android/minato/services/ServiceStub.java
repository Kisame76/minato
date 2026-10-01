package com.android.minato.services;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * A local stub for an AIDL interface (ADR-0045 decision 3): one {@link Proxy} per interface, since IActivityManager
 * alone has hundreds of methods. For each call its handler
 * <ol>
 *   <li>answers Object's methods by identity, and asBinder() with the Binder the stub is attached to;</li>
 *   <li>asks the permission gate, which throws SecurityException where the real service would (decision 6);</li>
 *   <li>hands the method to the answers object when it has a public method of the same name and parameter types
 *       (decision 4), and lets that method's exception reach the caller as it is;</li>
 *   <li>else logs the method once and returns its type's default: 0, false, '\0' or null, as AIDL's Default class
 *       does.</li>
 * </ol>
 * ART makes proxy classes without the JIT: their methods enter art_quick_proxy_invoke_handler, which the interpreter
 * calls as it calls native methods. Plain Java without android.*, so tools/framework-host-check.sh runs it on the
 * host's JVM.
 */
public final class ServiceStub implements InvocationHandler {
    /** Decision 6's check, before any answer: throws SecurityException to refuse. */
    public interface PermissionGate {
        void check(Method method, Object[] args);
    }

    /** Where the stub's lines go: android.util.Log on the device. */
    public interface Logger {
        void log(String line);
    }

    /** Methods logged once, across every stub of their interface. */
    private static final Set<Method> logged = ConcurrentHashMap.newKeySet();

    private static final Map<Class<?>, Object> DEFAULTS = new HashMap<>();
    static {
        DEFAULTS.put(boolean.class, false);
        DEFAULTS.put(char.class, '\0');
        DEFAULTS.put(byte.class, (byte) 0);
        DEFAULTS.put(short.class, (short) 0);
        DEFAULTS.put(int.class, 0);
        DEFAULTS.put(long.class, 0L);
        DEFAULTS.put(float.class, 0f);
        DEFAULTS.put(double.class, 0d);
    }

    private final Class<?> aidl;
    private final Object binder;
    private final Object answers;
    private final PermissionGate gate;
    private final Logger logger;
    /** The interface's methods that the answers object implements, found once at creation. */
    private final Map<Method, Method> answered = new HashMap<>();

    private ServiceStub(Class<?> aidl, Object binder, Object answers, PermissionGate gate, Logger logger) {
        this.aidl = aidl;
        this.binder = binder;
        this.answers = answers;
        this.gate = gate;
        this.logger = logger;
        if (answers == null) {
            return;
        }
        for (Method method : aidl.getMethods()) {
            Method answer;
            try {
                answer = answers.getClass().getMethod(method.getName(), method.getParameterTypes());
            } catch (NoSuchMethodException none) {
                continue;
            }
            if (answer.getReturnType() != method.getReturnType()) {
                throw new IllegalArgumentException(answers.getClass().getName() + "." + method.getName()
                        + " returns " + answer.getReturnType().getName() + ", not "
                        + method.getReturnType().getName());
            }
            // A package-private answers class has public methods that reflection may not call otherwise.
            answer.setAccessible(true);
            answered.put(method, answer);
        }
    }

    /**
     * A stub of {@code aidl}, an interface.
     *
     * @param binder  what asBinder() returns: the Binder the stub is attached to, or null
     * @param answers an object with public methods of the interface's names and parameter types, or null
     * @throws IllegalArgumentException when {@code aidl} is not an interface, or an answer's return type differs
     */
    public static <T> T create(Class<T> aidl, Object binder, Object answers, PermissionGate gate, Logger logger) {
        if (!aidl.isInterface()) {
            throw new IllegalArgumentException(aidl.getName() + " is not an interface");
        }
        ServiceStub handler = new ServiceStub(aidl, binder, answers, gate, logger);
        return aidl.cast(Proxy.newProxyInstance(aidl.getClassLoader(), new Class<?>[] {aidl}, handler));
    }

    /** For the host tests: every method logs again. */
    static void forgetLogged() {
        logged.clear();
    }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        Object[] arguments = args == null ? new Object[0] : args;
        if (method.getDeclaringClass() == Object.class) {
            switch (method.getName()) {
                case "equals": return proxy == arguments[0];
                case "hashCode": return System.identityHashCode(proxy);
                default: return "ServiceStub[" + aidl.getName() + "]";
            }
        }
        if (arguments.length == 0 && method.getName().equals("asBinder")) {
            return binder;
        }
        gate.check(method, arguments);
        Method answer = answered.get(method);
        if (answer != null) {
            try {
                return answer.invoke(answers, arguments);
            } catch (InvocationTargetException thrown) {
                throw thrown.getCause();
            }
        }
        Object value = DEFAULTS.get(method.getReturnType());
        if (logged.add(method)) {
            String returns = method.getReturnType() == void.class ? ""
                    : ", returns " + (value instanceof Character ? "'\\0'" : value);
            logger.log("ServiceStub: " + aidl.getName() + "." + method.getName() + "(" + parameters(method)
                    + ") is not implemented" + returns);
        }
        return value;
    }

    private static String parameters(Method method) {
        StringBuilder names = new StringBuilder();
        for (Class<?> type : method.getParameterTypes()) {
            if (names.length() > 0) {
                names.append(',');
            }
            names.append(type.getTypeName());
        }
        return names.toString();
    }
}
