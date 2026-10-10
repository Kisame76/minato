package io.github.kisame76.minato.test.gapscan;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Makes a dex file that holds only ID tables and class definitions, no code: the part of the format GapScan reads. Its
 * checksum and signature stay zero, and its tables are in the order they were added, which GapScan does not mind.
 */
final class DexMaker {
    private final Map<String, Integer> strings = new LinkedHashMap<>();
    private final Map<String, Integer> types = new LinkedHashMap<>();
    private final List<int[]> protos = new ArrayList<>();
    private final List<int[]> fields = new ArrayList<>();
    private final List<int[]> methods = new ArrayList<>();
    private final List<int[]> classes = new ArrayList<>();
    private final List<int[]> classMethods = new ArrayList<>();
    private final List<int[]> classFields = new ArrayList<>();

    private int string(String s) {
        return strings.computeIfAbsent(s, k -> strings.size());
    }

    int type(String descriptor) {
        string(descriptor);
        return types.computeIfAbsent(descriptor, k -> types.size());
    }

    int proto(String returns, String... parameters) {
        int[] proto = new int[parameters.length + 2];
        proto[0] = string("V");
        proto[1] = type(returns);
        for (int i = 0; i < parameters.length; i++) {
            proto[i + 2] = type(parameters[i]);
        }
        protos.add(proto);
        return protos.size() - 1;
    }

    int field(String owner, String type, String name) {
        fields.add(new int[] {type(owner), type(type), string(name)});
        return fields.size() - 1;
    }

    int method(String owner, int proto, String name) {
        methods.add(new int[] {type(owner), proto, string(name)});
        return methods.size() - 1;
    }

    /** A class the dex defines: its super type (null for none), its interfaces and the ids of the members it declares. */
    void define(String descriptor, String superType, String[] interfaces, int[] methodIds, int[] fieldIds) {
        int[] c = new int[2 + interfaces.length];
        c[0] = type(descriptor);
        c[1] = superType == null ? -1 : type(superType);
        for (int i = 0; i < interfaces.length; i++) {
            c[2 + i] = type(interfaces[i]);
        }
        classes.add(c);
        classMethods.add(methodIds);
        classFields.add(fieldIds);
    }

    byte[] build() {
        int stringsAt = 0x70;
        int typesAt = stringsAt + 4 * strings.size();
        int protosAt = typesAt + 4 * types.size();
        int fieldsAt = protosAt + 12 * protos.size();
        int methodsAt = fieldsAt + 8 * fields.size();
        int classesAt = methodsAt + 8 * methods.size();
        int dataAt = classesAt + 32 * classes.size();
        ByteArrayOutputStream data = new ByteArrayOutputStream();
        int[] stringOffsets = new int[strings.size()];
        for (Map.Entry<String, Integer> e : strings.entrySet()) {
            stringOffsets[e.getValue()] = dataAt + data.size();
            byte[] bytes = e.getKey().getBytes(StandardCharsets.UTF_8);
            data.write(bytes.length);
            data.write(bytes, 0, bytes.length);
            data.write(0);
        }
        List<Integer> protoLists = new ArrayList<>();
        for (int[] proto : protos) {
            protoLists.add(typeList(data, dataAt, Arrays.copyOfRange(proto, 2, proto.length)));
        }
        List<Integer> interfaceLists = new ArrayList<>();
        List<Integer> classData = new ArrayList<>();
        for (int i = 0; i < classes.size(); i++) {
            int[] c = classes.get(i);
            interfaceLists.add(typeList(data, dataAt, Arrays.copyOfRange(c, 2, c.length)));
            classData.add(classData(data, dataAt, classFields.get(i), classMethods.get(i)));
        }
        ByteBuffer b = ByteBuffer.allocate(dataAt + data.size()).order(ByteOrder.LITTLE_ENDIAN);
        b.put(new byte[] {'d', 'e', 'x', '\n', '0', '3', '5', 0});
        b.putInt(0x20, b.capacity());
        b.putInt(0x24, 0x70);
        b.putInt(0x28, 0x12345678);
        b.putInt(0x38, strings.size()).putInt(0x3c, stringsAt);
        b.putInt(0x40, types.size()).putInt(0x44, typesAt);
        b.putInt(0x48, protos.size()).putInt(0x4c, protosAt);
        b.putInt(0x50, fields.size()).putInt(0x54, fieldsAt);
        b.putInt(0x58, methods.size()).putInt(0x5c, methodsAt);
        b.putInt(0x60, classes.size()).putInt(0x64, classesAt);
        b.putInt(0x68, data.size()).putInt(0x6c, dataAt);
        for (int i = 0; i < stringOffsets.length; i++) {
            b.putInt(stringsAt + 4 * i, stringOffsets[i]);
        }
        for (Map.Entry<String, Integer> e : types.entrySet()) {
            b.putInt(typesAt + 4 * e.getValue(), strings.get(e.getKey()));
        }
        for (int i = 0; i < protos.size(); i++) {
            int at = protosAt + 12 * i;
            b.putInt(at, protos.get(i)[0]).putInt(at + 4, protos.get(i)[1]).putInt(at + 8, protoLists.get(i));
        }
        for (int i = 0; i < fields.size(); i++) {
            int[] f = fields.get(i);
            b.putShort(fieldsAt + 8 * i, (short) f[0]).putShort(fieldsAt + 8 * i + 2, (short) f[1])
                    .putInt(fieldsAt + 8 * i + 4, f[2]);
        }
        for (int i = 0; i < methods.size(); i++) {
            int[] m = methods.get(i);
            b.putShort(methodsAt + 8 * i, (short) m[0]).putShort(methodsAt + 8 * i + 2, (short) m[1])
                    .putInt(methodsAt + 8 * i + 4, m[2]);
        }
        for (int i = 0; i < classes.size(); i++) {
            int at = classesAt + 32 * i;
            b.putInt(at, classes.get(i)[0]).putInt(at + 8, classes.get(i)[1]).putInt(at + 12, interfaceLists.get(i))
                    .putInt(at + 24, classData.get(i));
        }
        b.position(dataAt);
        b.put(data.toByteArray());
        return b.array();
    }

    /** A type_list of `typeIds` in `data`, aligned to 4, as the offset in the file; 0 for none. */
    private static int typeList(ByteArrayOutputStream data, int dataAt, int[] typeIds) {
        if (typeIds.length == 0) {
            return 0;
        }
        while ((dataAt + data.size()) % 4 != 0) {
            data.write(0);
        }
        int at = dataAt + data.size();
        ByteBuffer list = ByteBuffer.allocate(4 + 2 * typeIds.length).order(ByteOrder.LITTLE_ENDIAN);
        list.putInt(typeIds.length);
        for (int id : typeIds) {
            list.putShort((short) id);
        }
        data.write(list.array(), 0, list.capacity());
        return at;
    }

    /** A class_data_item with fields as instance fields and methods as virtual methods, ids ascending. */
    private static int classData(ByteArrayOutputStream data, int dataAt, int[] fieldIds, int[] methodIds) {
        if (fieldIds.length == 0 && methodIds.length == 0) {
            return 0;
        }
        int at = dataAt + data.size();
        uleb(data, 0);
        uleb(data, fieldIds.length);
        uleb(data, 0);
        uleb(data, methodIds.length);
        int[] sorted = fieldIds.clone();
        Arrays.sort(sorted);
        for (int i = 0, previous = 0; i < sorted.length; i++) {
            uleb(data, sorted[i] - previous);
            uleb(data, 1);
            previous = sorted[i];
        }
        sorted = methodIds.clone();
        Arrays.sort(sorted);
        for (int i = 0, previous = 0; i < sorted.length; i++) {
            uleb(data, sorted[i] - previous);
            uleb(data, 1);
            uleb(data, 0);
            previous = sorted[i];
        }
        return at;
    }

    private static void uleb(ByteArrayOutputStream out, int value) {
        do {
            int each = value & 0x7f;
            value >>>= 7;
            out.write(value != 0 ? each | 0x80 : each);
        } while (value != 0);
    }
}
