package io.github.kisame76.minato.test.json;

import android.app.Application;
import android.util.JsonReader;
import android.util.JsonToken;
import android.util.JsonWriter;
import android.util.Log;
import android.util.MalformedJsonException;

import java.io.IOException;
import java.io.StringReader;
import java.io.StringWriter;
import java.math.BigDecimal;

/**
 * android.util.JsonWriter and JsonReader, which Firebase's Crashlytics and Installations use (the Unity test APK probe): a
 * document written compact and indented, with its escapes and refusals, then read back as tokens and as typed values,
 * strict and lenient, and the messages of what is malformed or ends too early.
 */
public class JsonApplication extends Application {
    static final String TAG = "json-test";

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Body {
        void run(JsonWriter writer) throws IOException;
    }

    private interface Read {
        String run(JsonReader reader) throws IOException;
    }

    /** A line of text with its control characters spelled out, as one logcat line. */
    private static String shown(String text) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c == '\n') {
                out.append("\\n");
            } else if (c == '\r') {
                out.append("\\r");
            } else if (c < ' ' || c > '~') {
                out.append(String.format("<%04x>", (int) c));
            } else {
                out.append(c);
            }
        }
        return out.toString();
    }

    private static String failure(Exception e) {
        return e.getClass().getName() + ": " + e.getMessage();
    }

    private static String write(String indent, boolean lenient, Body body) {
        StringWriter out = new StringWriter();
        JsonWriter writer = new JsonWriter(out);
        try {
            if (indent != null) {
                writer.setIndent(indent);
            }
            writer.setLenient(lenient);
            body.run(writer);
            writer.close();
            return shown(out.toString());
        } catch (Exception e) {
            return shown(out.toString()) + " then " + failure(e);
        }
    }

    private static void document(JsonWriter writer) throws IOException {
        writer.beginObject();
        writer.name("name").value("Minato");
        writer.name("kanji").value("\u6e2f");
        writer.name("count").value(3L);
        writer.name("ratio").value(0.5);
        writer.name("big").value(1e10);
        writer.name("max").value(Long.MAX_VALUE);
        writer.name("ok").value(true);
        writer.name("nothing").nullValue();
        writer.name("list").beginArray().value(1).value(2.5).value("x").value((String) null).beginObject()
                .name("deep").beginArray().endArray().endObject().endArray();
        writer.name("empty").beginObject().endObject();
        writer.name("number").value(new BigDecimal("12345678901234567890.5"));
        writer.endObject();
    }

    @Override
    public void onCreate() {
        super.onCreate();
        writing();
        reading();
        log("done");
    }

    private void writing() {
        log("compact: " + write(null, false, JsonApplication::document));
        log("indented: " + write("  ", false, JsonApplication::document));
        String text = "quote \" back \\ nl \n tab \t cr \r bs \b ff \f nul \u0000 us \u001f del \u007f ls \u2028 ps \u2029"
                + " html <&>=' slash / kanji \u6e2f";
        log("escapes: " + write(null, false, w -> w.beginArray().value(text).endArray()));
        log("a name's escapes: " + write(null, false,
                w -> w.beginObject().name("a\"b\n").value(1).endObject()));
        log("NaN strict: " + write(null, false, w -> w.beginArray().value(Double.NaN)));
        log("NaN lenient: " + write(null, true,
                w -> w.beginArray().value(Double.NaN).value(Double.NEGATIVE_INFINITY).endArray()));
        log("a Number's text: " + write(null, false,
                w -> w.beginArray().value((Number) 7).value((Number) 1.5f).value((Number) null).endArray()));
        log("name without an object: " + write(null, false, w -> w.beginArray().name("a")));
        log("name twice: " + write(null, false, w -> w.beginObject().name("a").name("b")));
        log("value without a name: " + write(null, false, w -> w.beginObject().value(1)));
        log("end of the wrong kind: " + write(null, false, w -> w.beginArray().endObject()));
        log("a null name: " + write(null, false, w -> w.beginObject().name(null)));
        log("two top-level values: " + write(null, false, w -> w.beginArray().endArray().beginArray()));
        log("two top-level values, lenient: " + write(null, true, w -> w.beginArray().endArray().beginArray().endArray()));
        log("a top-level null: " + write(null, false, w -> w.nullValue()));
        log("closed while open: " + write(null, false, w -> w.beginArray().value(1)));
        log("nothing written: " + write(null, false, w -> { }));
        log("after close: " + write(null, false, w -> {
            w.beginArray().endArray();
            w.close();
            w.value(2);
        }));
        log("a lenient indent: " + write("\t", true,
                w -> w.beginObject().name("a").beginArray().value(1).value(2).endArray().endObject()));
    }

    private static String reading(String json, boolean lenient, Read read) {
        JsonReader reader = new JsonReader(new StringReader(json));
        reader.setLenient(lenient);
        try {
            return read.run(reader);
        } catch (Exception e) {
            return failure(e);
        }
    }

    private static String tokens(JsonReader reader) throws IOException {
        StringBuilder out = new StringBuilder();
        while (true) {
            JsonToken token = reader.peek();
            out.append(token);
            switch (token) {
                case BEGIN_ARRAY:
                    reader.beginArray();
                    break;
                case END_ARRAY:
                    reader.endArray();
                    break;
                case BEGIN_OBJECT:
                    reader.beginObject();
                    break;
                case END_OBJECT:
                    reader.endObject();
                    break;
                case NAME:
                    out.append('=').append(reader.nextName());
                    break;
                case STRING:
                    out.append('=').append(shown(reader.nextString()));
                    break;
                case NUMBER:
                    out.append('=').append(reader.nextString());
                    break;
                case BOOLEAN:
                    out.append('=').append(reader.nextBoolean());
                    break;
                case NULL:
                    reader.nextNull();
                    break;
                case END_DOCUMENT:
                    return out.toString();
                default:
                    return out + " ?";
            }
            out.append(' ');
        }
    }

    private void reading() {
        String small = "{\"a\":[1,2.5,\"x\",true,null,{\"b\":-0}],\"c\":\"d\\u6e2f\\n\"}";
        log("tokens: " + reading(small, false, JsonApplication::tokens));
        log("typed: " + reading("[1,-2,3000000000,1.0,1e2,\"7\",\"1.5\",12345678901234567890,true,null]", false, r -> {
            StringBuilder out = new StringBuilder();
            r.beginArray();
            out.append(r.nextInt()).append(' ').append(r.nextInt()).append(' ').append(r.nextLong()).append(' ');
            out.append(r.nextLong()).append(' ').append(r.nextLong()).append(' ').append(r.nextInt()).append(' ');
            out.append(r.nextDouble()).append(' ').append(r.nextDouble()).append(' ').append(r.nextBoolean());
            r.nextNull();
            r.endArray();
            return out.toString();
        }));
        log("an int too large: " + reading("[3000000000]", false, r -> {
            r.beginArray();
            return "" + r.nextInt();
        }));
        log("an int from 1.5: " + reading("[1.5]", false, r -> {
            r.beginArray();
            return "" + r.nextInt();
        }));
        log("a long from a string: " + reading("[\"x\"]", false, r -> {
            r.beginArray();
            return "" + r.nextLong();
        }));
        log("a string where an array is: " + reading("[1]", false, r -> r.nextString()));
        log("a name where a value is: " + reading("{\"a\":1}", false, r -> {
            r.beginObject();
            r.nextName();
            return r.nextName();
        }));
        log("a boolean from a string: " + reading("[\"true\"]", false, r -> {
            r.beginArray();
            return "" + r.nextBoolean();
        }));
        log("skipValue: " + reading("[{\"a\":[1,{\"b\":2}]},\"y\",[3],4]", false, r -> {
            r.beginArray();
            r.skipValue();
            r.skipValue();
            r.skipValue();
            return r.peek() + " " + r.nextInt() + " " + r.hasNext() + " " + r.peek();
        }));
        log("hasNext: " + reading("{\"a\":[],\"b\":{}}", false, r -> {
            StringBuilder out = new StringBuilder();
            r.beginObject();
            out.append(r.hasNext());
            r.nextName();
            r.beginArray();
            out.append(r.hasNext());
            r.endArray();
            r.nextName();
            r.beginObject();
            out.append(r.hasNext());
            r.endObject();
            out.append(r.hasNext());
            r.endObject();
            return out.append(r.peek()).toString();
        }));
        log("unicode: " + reading("[\"\\u6e2f\\ud83d\\ude00 \\/ \\b\\f\"]", false, r -> {
            r.beginArray();
            return shown(r.nextString());
        }));
        log("a short escape: " + reading("[\"\\u12\"]", false, r -> {
            r.beginArray();
            return r.nextString();
        }));
        log("a bad escape: " + reading("[\"\\q\"]", false, r -> {
            r.beginArray();
            return r.nextString();
        }));
        log("a raw newline in a string: " + reading("[\"a\nb\"]", false, r -> {
            r.beginArray();
            return shown(r.nextString());
        }));
        log("unterminated: " + reading("[\"abc", false, r -> {
            r.beginArray();
            return r.nextString();
        }));
        log("strict, unquoted: " + reading("{a:1}", false, JsonApplication::tokens));
        log("lenient, unquoted: " + reading("{a:1,'b':'two';c=>[x,y]=z}", true, JsonApplication::tokens));
        log("lenient, comments: " + reading("[1 /* two */, 3 // four\n, 5 # six\n]", true, JsonApplication::tokens));
        log("lenient, a prefix: " + reading(")]}'\n[1]", true, JsonApplication::tokens));
        log("strict, a prefix: " + reading(")]}'\n[1]", false, JsonApplication::tokens));
        log("lenient, several values: " + reading("1 2 \"three\"", true, JsonApplication::tokens));
        log("strict, several values: " + reading("1 2", false, JsonApplication::tokens));
        log("strict, a single quote: " + reading("['a']", false, JsonApplication::tokens));
        log("strict, a trailing comma: " + reading("[1,]", false, JsonApplication::tokens));
        log("lenient, a trailing comma: " + reading("[1,]", true, JsonApplication::tokens));
        log("strict, NaN: " + reading("[NaN]", false, r -> {
            r.beginArray();
            return "" + r.nextDouble();
        }));
        log("lenient, NaN: " + reading("[NaN,Infinity]", true, r -> {
            r.beginArray();
            return r.nextDouble() + " " + r.nextDouble();
        }));
        log("empty input: " + reading("", false, r -> "" + r.peek()));
        log("input cut off: " + reading("[1,", false, JsonApplication::tokens));
        log("input cut off in an object: " + reading("{\"a\"", false, JsonApplication::tokens));
        log("a second line: " + reading("[\n  1,\n  oops\n]", false, JsonApplication::tokens));
        log("a leading plus and zeros: " + reading("[+1, 007, -0.0e0, .5]", true, JsonApplication::tokens));
        log("a malformed number: " + reading("[1.2.3]", false, JsonApplication::tokens));
        log("close and read: " + reading("[1]", false, r -> {
            r.close();
            return "" + r.peek();
        }));
        log("deep: " + reading(new String(new char[200]).replace("\0", "[") + new String(new char[200]).replace("\0", "]"),
                false, r -> {
            int depth = 0;
            while (r.peek() == JsonToken.BEGIN_ARRAY) {
                r.beginArray();
                depth++;
            }
            return depth + " " + r.peek();
        }));
        log("toString: " + reading("[1, 2, 3]", false, r -> {
            r.beginArray();
            r.nextInt();
            return r.toString().replace(r.getClass().getSimpleName(), "JsonReader");
        }));
        log("lenient flags: " + new JsonReader(new StringReader("")).isLenient() + " "
                + new JsonWriter(new StringWriter()).isLenient());
        log("a written document read back: " + reading(write(null, false, JsonApplication::document),
                false, JsonApplication::tokens));
        log("a malformed exception: " + (new MalformedJsonException("m") instanceof IOException));
    }
}
