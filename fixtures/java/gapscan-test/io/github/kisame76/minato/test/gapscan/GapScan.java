package io.github.kisame76.minato.test.gapscan;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * The gap scanner, a developer diagnostic: it reads the ID tables and the class definitions of an APK's classes*.dex
 * (names only, never a code body) and says which platform classes, methods and fields the running boot classpath lacks,
 * so that an app's missing APIs are found up front and not one per retest.
 * <ul>
 * <li>Platform means android.*, java.*, javax.*, dalvik.* and the legacy org.* packages of the Android platform.
 * <li>A type the APK defines itself, such as a bundled org.json, is no gap.
 * <li>A member referenced on an app's class is looked for up the app's own classes, then in the first platform class,
 * as ART resolves it, with superclasses and interfaces.
 * <li>The boot classpath is searched by reflection, so on Android a hidden member shows as missing, and on Minato not.
 * </ul>
 */
public final class GapScan {
    private static final String[] PLATFORM = {"android.", "java.", "javax.", "dalvik.", "org.apache.http.", "org.json.",
            "org.xml.", "org.w3c.", "org.xmlpull.", "junit."};
    private static final Pattern DEX = Pattern.compile("classes\\d*\\.dex");
    private static final int FOUND = 0;
    private static final int MISSING = 1;
    private static final int UNKNOWN = 2;

    private final Map<String, AppClass> app = new HashMap<>();
    private final Map<String, Class<?>> classes = new HashMap<>();
    private final Set<String> absent = new HashSet<>();
    private final Map<String, String> broken = new TreeMap<>();
    private final Map<Class<?>, Set<String>> methodsOf = new HashMap<>();
    private final Map<Class<?>, Set<String>> fieldsOf = new HashMap<>();
    private final Map<String, Integer> missingClasses = new TreeMap<>();
    private final Map<String, Integer> missingMethods = new TreeMap<>();
    private final Map<String, Integer> missingFields = new TreeMap<>();

    /** What an APK defines itself: its superclass, interfaces and the keys of its declared members. */
    private static final class AppClass {
        String superclass;
        final List<String> interfaces = new ArrayList<>();
        final Set<String> methods = new HashSet<>();
        final Set<String> fields = new HashSet<>();
    }

    private GapScan() {
    }

    /** What a scan found: the counts of what is missing and the lines to show, the summary line first. */
    public static final class Report {
        public final int classes;
        public final int methods;
        public final int fields;
        public final int unreadable;
        public final List<String> lines;

        Report(int classes, int methods, int fields, int unreadable, List<String> lines) {
            this.classes = classes;
            this.methods = methods;
            this.fields = fields;
            this.unreadable = unreadable;
            this.lines = lines;
        }
    }

    /** The scan of the dex files in `apk`: the summary line first, then the missing, by package. */
    public static Report scanApk(String apk) throws IOException {
        List<byte[]> dexes = new ArrayList<>();
        try (ZipFile zip = new ZipFile(apk)) {
            for (java.util.Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                ZipEntry entry = e.nextElement();
                if (DEX.matcher(entry.getName()).matches()) {
                    try (InputStream in = zip.getInputStream(entry)) {
                        ByteArrayOutputStream out = new ByteArrayOutputStream();
                        byte[] buffer = new byte[65536];
                        for (int count; (count = in.read(buffer)) > 0; ) {
                            out.write(buffer, 0, count);
                        }
                        dexes.add(out.toByteArray());
                    }
                }
            }
        }
        return scan(dexes);
    }

    /** The scan of dex files given as bytes. */
    public static Report scan(List<byte[]> dexFiles) {
        GapScan scan = new GapScan();
        List<Dex> dexes = new ArrayList<>();
        for (byte[] bytes : dexFiles) {
            Dex dex = new Dex(bytes);
            dexes.add(dex);
            dex.readClasses(scan.app);
        }
        for (Dex dex : dexes) {
            scan.references(dex);
        }
        return scan.report();
    }

    private static boolean platform(String name) {
        for (String prefix : PLATFORM) {
            if (name.startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }

    /** "Ljava/lang/String;" as java.lang.String, an array as its element type, a primitive as null. */
    private static String className(String descriptor) {
        int at = 0;
        while (at < descriptor.length() && descriptor.charAt(at) == '[') {
            at++;
        }
        if (at >= descriptor.length() || descriptor.charAt(at) != 'L') {
            return null;
        }
        return descriptor.substring(at + 1, descriptor.length() - 1).replace('/', '.');
    }

    private static String descriptor(Class<?> type) {
        if (type.isArray()) {
            return "[" + descriptor(type.getComponentType());
        }
        if (type.isPrimitive()) {
            return type == int.class ? "I" : type == long.class ? "J" : type == boolean.class ? "Z"
                    : type == byte.class ? "B" : type == char.class ? "C" : type == short.class ? "S"
                    : type == float.class ? "F" : type == double.class ? "D" : "V";
        }
        return "L" + type.getName().replace('.', '/') + ";";
    }

    private static void count(Map<String, Integer> map, String key) {
        map.merge(key, 1, Integer::sum);
    }

    private void references(Dex dex) {
        for (int i = 0; i < dex.types; i++) {
            String name = className(dex.type(i));
            if (name != null && platform(name) && !app.containsKey("L" + name.replace('.', '/') + ";")
                    && load(name) == null) {
                count(missingClasses, name);
            }
        }
        for (int i = 0; i < dex.methods; i++) {
            String owner = dex.methodOwner(i);
            if (owner.startsWith("[")) {
                continue;
            }
            String key = dex.methodName(i) + dex.methodSignature(i);
            member(owner, true, key, dex.methodName(i));
        }
        for (int i = 0; i < dex.fields; i++) {
            String owner = dex.fieldOwner(i);
            if (owner.startsWith("[")) {
                continue;
            }
            member(owner, false, dex.fieldName(i) + ":" + dex.fieldType(i), dex.fieldName(i));
        }
    }

    private void member(String owner, boolean method, String key, String name) {
        String ownerName = className(owner);
        if (ownerName == null) {
            return;
        }
        boolean own = app.containsKey(owner);
        if (!own && !platform(ownerName)) {
            return;
        }
        // Signature-polymorphic methods take any signature.
        if (method && (ownerName.equals("java.lang.invoke.MethodHandle") || ownerName.equals("java.lang.invoke.VarHandle"))) {
            return;
        }
        if (!own && load(ownerName) == null) {
            // A class that is not there: its members count with it.
            count(missingClasses, ownerName);
            return;
        }
        if (lookup(owner, method, key) != MISSING) {
            return;
        }
        String shown = own ? nearestPlatform(owner) + "." + key + " [via " + ownerName + "]" : ownerName + "." + key;
        count(method ? missingMethods : missingFields, shown);
    }

    /** The first platform superclass of an app's class, for the package a missing member is grouped under. */
    private String nearestPlatform(String descriptor) {
        for (int depth = 0; depth < 64; depth++) {
            AppClass c = app.get(descriptor);
            if (c == null || c.superclass == null) {
                break;
            }
            descriptor = c.superclass;
        }
        String name = className(descriptor);
        return name == null ? "java.lang.Object" : name;
    }

    private int lookup(String descriptor, boolean method, String key) {
        AppClass own = app.get(descriptor);
        if (own != null) {
            if ((method ? own.methods : own.fields).contains(key)) {
                return FOUND;
            }
            boolean unknown = false;
            List<String> parents = new ArrayList<>(own.interfaces);
            if (own.superclass != null) {
                parents.add(0, own.superclass);
            }
            for (String parent : parents) {
                int result = lookup(parent, method, key);
                if (result == FOUND) {
                    return FOUND;
                }
                unknown |= result == UNKNOWN;
            }
            return unknown ? UNKNOWN : MISSING;
        }
        String name = className(descriptor);
        if (name == null || !platform(name)) {
            return UNKNOWN;
        }
        Class<?> c = load(name);
        return c == null ? UNKNOWN : search(c, method, key, new HashSet<>());
    }

    private int search(Class<?> c, boolean method, String key, Set<Class<?>> seen) {
        if (c == null || !seen.add(c)) {
            return MISSING;
        }
        Set<String> declared = declared(c, method);
        boolean unknown = false;
        if (declared == null) {
            // The class's members cannot all be read (a type in a signature is missing): ask for this one alone.
            int own = direct(c, method, key);
            if (own == FOUND) {
                return FOUND;
            }
            unknown = own == UNKNOWN;
        } else if (declared.contains(key)) {
            return FOUND;
        }
        List<Class<?>> parents = new ArrayList<>();
        parents.add(c.getSuperclass());
        for (Class<?> each : c.getInterfaces()) {
            parents.add(each);
        }
        if (c.isInterface()) {
            parents.add(Object.class);
        }
        for (Class<?> parent : parents) {
            int result = search(parent, method, key, seen);
            if (result == FOUND) {
                return FOUND;
            }
            unknown |= result == UNKNOWN;
        }
        return unknown ? UNKNOWN : MISSING;
    }

    /** `key` looked for by name and parameter types alone, which does not read the class's other signatures. */
    private int direct(Class<?> c, boolean method, String key) {
        try {
            if (!method) {
                int colon = key.indexOf(':');
                Field field = c.getDeclaredField(key.substring(0, colon));
                return descriptor(field.getType()).equals(key.substring(colon + 1)) ? FOUND : MISSING;
            }
            int open = key.indexOf('(');
            int close = key.indexOf(')');
            List<Class<?>> parameters = new ArrayList<>();
            for (int at = open + 1; at < close; ) {
                int end = at;
                while (key.charAt(end) == '[') {
                    end++;
                }
                end = key.charAt(end) == 'L' ? key.indexOf(';', end) + 1 : end + 1;
                Class<?> type = typeOf(key.substring(at, end));
                if (type == null) {
                    return UNKNOWN;
                }
                parameters.add(type);
                at = end;
            }
            String name = key.substring(0, open);
            Class<?>[] types = parameters.toArray(new Class<?>[0]);
            if (name.equals("<init>")) {
                c.getDeclaredConstructor(types);
                return FOUND;
            }
            Method found = c.getDeclaredMethod(name, types);
            return descriptor(found.getReturnType()).equals(key.substring(close + 1)) ? FOUND : MISSING;
        } catch (NoSuchMethodException | NoSuchFieldException e) {
            return MISSING;
        } catch (LinkageError e) {
            return UNKNOWN;
        }
    }

    /** The class of a type descriptor on the boot classpath, or null for an app's own or a missing one. */
    private Class<?> typeOf(String descriptor) {
        switch (descriptor.charAt(0)) {
            case 'I': return int.class;
            case 'J': return long.class;
            case 'Z': return boolean.class;
            case 'B': return byte.class;
            case 'C': return char.class;
            case 'S': return short.class;
            case 'F': return float.class;
            case 'D': return double.class;
            case 'V': return void.class;
            case '[':
                Class<?> element = typeOf(descriptor.substring(1));
                return element == null ? null : java.lang.reflect.Array.newInstance(element, 0).getClass();
            default:
                return load(className(descriptor));
        }
    }

    /** The keys of the members `c` declares, or null while the class cannot be read (a missing class in a signature). */
    private Set<String> declared(Class<?> c, boolean method) {
        Map<Class<?>, Set<String>> cache = method ? methodsOf : fieldsOf;
        if (cache.containsKey(c)) {
            return cache.get(c);
        }
        Set<String> keys = new HashSet<>();
        try {
            if (method) {
                for (Method each : c.getDeclaredMethods()) {
                    StringBuilder key = new StringBuilder(each.getName()).append('(');
                    for (Class<?> type : each.getParameterTypes()) {
                        key.append(descriptor(type));
                    }
                    keys.add(key.append(')').append(descriptor(each.getReturnType())).toString());
                }
                for (Constructor<?> each : c.getDeclaredConstructors()) {
                    StringBuilder key = new StringBuilder("<init>(");
                    for (Class<?> type : each.getParameterTypes()) {
                        key.append(descriptor(type));
                    }
                    keys.add(key.append(")V").toString());
                }
            } else {
                for (Field each : c.getDeclaredFields()) {
                    keys.add(each.getName() + ":" + descriptor(each.getType()));
                }
            }
        } catch (LinkageError e) {
            broken.put(c.getName(), e.getClass().getSimpleName() + ": " + e.getMessage());
            keys = null;
        }
        cache.put(c, keys);
        return keys;
    }

    private Class<?> load(String name) {
        if (classes.containsKey(name)) {
            return classes.get(name);
        }
        Class<?> c = null;
        try {
            // The boot classpath alone: what an app's own loader leaves to the platform.
            c = Class.forName(name, false, null);
        } catch (ClassNotFoundException | LinkageError e) {
            absent.add(name);
        }
        classes.put(name, c);
        return c;
    }

    private Report report() {
        List<String> lines = new ArrayList<>();
        lines.add("gapscan: " + missingClasses.size() + " missing classes, " + missingMethods.size()
                + " missing methods, " + missingFields.size() + " missing fields"
                + (broken.isEmpty() ? "" : ", " + broken.size() + " classes whose members cannot be read"));
        Map<String, List<String>> byPackage = new TreeMap<>();
        for (Map.Entry<String, Integer> e : missingClasses.entrySet()) {
            group(byPackage, e.getKey(), "class " + e.getKey() + " x" + e.getValue());
        }
        for (Map.Entry<String, Integer> e : missingMethods.entrySet()) {
            group(byPackage, e.getKey(), "method " + e.getKey() + " x" + e.getValue());
        }
        for (Map.Entry<String, Integer> e : missingFields.entrySet()) {
            group(byPackage, e.getKey(), "field " + e.getKey() + " x" + e.getValue());
        }
        for (Map.Entry<String, String> e : broken.entrySet()) {
            group(byPackage, e.getKey(), "unreadable " + e.getKey() + ": " + e.getValue());
        }
        for (Map.Entry<String, List<String>> e : byPackage.entrySet()) {
            lines.add("package " + e.getKey() + " (" + e.getValue().size() + ")");
            for (String item : e.getValue()) {
                lines.add("  " + item);
            }
        }
        return new Report(missingClasses.size(), missingMethods.size(), missingFields.size(), broken.size(), lines);
    }

    /** `item` under the package of the class in `qualified`, a class name or "class.member...". */
    private static void group(Map<String, List<String>> byPackage, String qualified, String item) {
        String head = qualified;
        int paren = head.indexOf('(');
        int colon = head.indexOf(':');
        int cut = paren >= 0 ? paren : colon >= 0 ? colon : head.length();
        head = head.substring(0, cut);
        int dot = head.lastIndexOf('.');
        // For a member, "pkg.Class.name" has the class before the last dot; for a class, "pkg.Class" has the package.
        String owner = item.startsWith("class ") || item.startsWith("unreadable ") ? head : head.substring(0, Math.max(dot, 0));
        int last = owner.lastIndexOf('.');
        String pkg = last < 0 ? "(default)" : owner.substring(0, last);
        byPackage.computeIfAbsent(pkg, k -> new ArrayList<>()).add(item);
    }

    /** One dex file's tables, read by offset; strings are decoded when asked. */
    static final class Dex {
        private final ByteBuffer b;
        private final String[] strings;
        private final int stringOff;
        private final int typeOff;
        private final int protoOff;
        private final int fieldOff;
        private final int methodOff;
        private final int classOff;
        final int types;
        final int fields;
        final int methods;
        private final int classDefs;

        Dex(byte[] data) {
            b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
            if (data.length < 0x70 || data[0] != 'd' || data[1] != 'e' || data[2] != 'x' || data[3] != '\n') {
                throw new IllegalArgumentException("not a dex file");
            }
            strings = new String[b.getInt(0x38)];
            stringOff = b.getInt(0x3c);
            types = b.getInt(0x40);
            typeOff = b.getInt(0x44);
            protoOff = b.getInt(0x4c);
            fields = b.getInt(0x50);
            fieldOff = b.getInt(0x54);
            methods = b.getInt(0x58);
            methodOff = b.getInt(0x5c);
            classDefs = b.getInt(0x60);
            classOff = b.getInt(0x64);
        }

        private String string(int index) {
            String s = strings[index];
            if (s == null) {
                int at = b.getInt(stringOff + 4 * index);
                while ((b.get(at++) & 0x80) != 0) {
                    // The UTF-16 length's ULEB128, which the bytes' end makes needless.
                }
                StringBuilder out = new StringBuilder();
                for (int c; (c = b.get(at++) & 0xff) != 0; ) {
                    if (c < 0x80) {
                        out.append((char) c);
                    } else if ((c & 0xe0) == 0xc0) {
                        out.append((char) (((c & 0x1f) << 6) | (b.get(at++) & 0x3f)));
                    } else {
                        int second = b.get(at++) & 0x3f;
                        out.append((char) (((c & 0x0f) << 12) | (second << 6) | (b.get(at++) & 0x3f)));
                    }
                }
                s = out.toString();
                strings[index] = s;
            }
            return s;
        }

        String type(int index) {
            return string(b.getInt(typeOff + 4 * index));
        }

        private String proto(int index) {
            int at = protoOff + 12 * index;
            StringBuilder out = new StringBuilder("(");
            int list = b.getInt(at + 8);
            if (list != 0) {
                for (int i = 0, n = b.getInt(list); i < n; i++) {
                    out.append(type(b.getShort(list + 4 + 2 * i) & 0xffff));
                }
            }
            return out.append(')').append(type(b.getInt(at + 4))).toString();
        }

        String methodOwner(int i) {
            return type(b.getShort(methodOff + 8 * i) & 0xffff);
        }

        String methodSignature(int i) {
            return proto(b.getShort(methodOff + 8 * i + 2) & 0xffff);
        }

        String methodName(int i) {
            return string(b.getInt(methodOff + 8 * i + 4));
        }

        String fieldOwner(int i) {
            return type(b.getShort(fieldOff + 8 * i) & 0xffff);
        }

        String fieldType(int i) {
            return type(b.getShort(fieldOff + 8 * i + 2) & 0xffff);
        }

        String fieldName(int i) {
            return string(b.getInt(fieldOff + 8 * i + 4));
        }

        private int[] position = new int[1];

        private int uleb() {
            int result = 0;
            for (int shift = 0; ; shift += 7) {
                int each = b.get(position[0]++) & 0xff;
                result |= (each & 0x7f) << shift;
                if ((each & 0x80) == 0) {
                    return result;
                }
            }
        }

        /** What this dex defines: each class, its superclass, interfaces and declared members. */
        void readClasses(Map<String, AppClass> into) {
            for (int i = 0; i < classDefs; i++) {
                int at = classOff + 32 * i;
                AppClass c = new AppClass();
                String self = type(b.getInt(at));
                int superIndex = b.getInt(at + 8);
                c.superclass = superIndex == -1 ? null : type(superIndex);
                int list = b.getInt(at + 12);
                if (list != 0) {
                    for (int k = 0, n = b.getInt(list); k < n; k++) {
                        c.interfaces.add(type(b.getShort(list + 4 + 2 * k) & 0xffff));
                    }
                }
                int data = b.getInt(at + 24);
                if (data != 0) {
                    position[0] = data;
                    int staticFields = uleb();
                    int instanceFields = uleb();
                    int directMethods = uleb();
                    int virtualMethods = uleb();
                    for (int k = 0, index = 0, n = staticFields + instanceFields; k < n; k++) {
                        if (k == staticFields) {
                            index = 0;
                        }
                        index += uleb();
                        uleb();
                        c.fields.add(fieldName(index) + ":" + fieldType(index));
                    }
                    for (int k = 0, index = 0, n = directMethods + virtualMethods; k < n; k++) {
                        if (k == directMethods) {
                            index = 0;
                        }
                        index += uleb();
                        uleb();
                        uleb();
                        c.methods.add(methodName(index) + methodSignature(index));
                    }
                }
                into.put(self, c);
            }
        }
    }
}
