// jni-fields-test's JNI library: field lookups through JNI as Unity's AndroidJNI makes them, each answered as a line
// for the Java side to log: the field's ID found, or the pending exception's class and message, which it clears.
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define PREFIX(name) Java_io_github_kisame76_minato_test_jnifields_JniFieldsApplication_##name

extern JavaVM *the_vm;
JavaVM *the_vm;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    the_vm = vm;
    return JNI_VERSION_1_6;
}

// The pending exception as "<class>: <message>", cleared; "no exception" when none is pending.
static void take_exception(JNIEnv *env, char *out, size_t size) {
    jthrowable thrown = (*env)->ExceptionOccurred(env);
    if (thrown == NULL) {
        snprintf(out, size, "no exception");
        return;
    }
    (*env)->ExceptionClear(env);
    jclass throwable = (*env)->FindClass(env, "java/lang/Throwable");
    jclass class_class = (*env)->FindClass(env, "java/lang/Class");
    jmethodID get_class = (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/Object"), "getClass",
                                              "()Ljava/lang/Class;");
    jmethodID get_name = (*env)->GetMethodID(env, class_class, "getName", "()Ljava/lang/String;");
    jmethodID get_message = (*env)->GetMethodID(env, throwable, "getMessage", "()Ljava/lang/String;");
    jstring name = (jstring)(*env)->CallObjectMethod(env, (*env)->CallObjectMethod(env, thrown, get_class), get_name);
    jstring message = (jstring)(*env)->CallObjectMethod(env, thrown, get_message);
    const char *name_text = (*env)->GetStringUTFChars(env, name, NULL);
    const char *message_text = message != NULL ? (*env)->GetStringUTFChars(env, message, NULL) : "null";
    snprintf(out, size, "%s: %s", name_text, message_text);
    (*env)->ReleaseStringUTFChars(env, name, name_text);
    if (message != NULL) (*env)->ReleaseStringUTFChars(env, message, message_text);
}

static jstring lookup(JNIEnv *env, jclass type, jstring name, jstring signature, int is_static) {
    const char *name_text = (*env)->GetStringUTFChars(env, name, NULL);
    const char *signature_text = (*env)->GetStringUTFChars(env, signature, NULL);
    jfieldID field = is_static ? (*env)->GetStaticFieldID(env, type, name_text, signature_text)
                               : (*env)->GetFieldID(env, type, name_text, signature_text);
    char line[512];
    if (field != NULL) {
        snprintf(line, sizeof line, "found");
    } else {
        take_exception(env, line, sizeof line);
    }
    (*env)->ReleaseStringUTFChars(env, name, name_text);
    (*env)->ReleaseStringUTFChars(env, signature, signature_text);
    return (*env)->NewStringUTF(env, line);
}

JNIEXPORT jstring JNICALL PREFIX(staticLookup)(JNIEnv *env, jclass self, jclass type, jstring name, jstring signature) {
    (void)self;
    return lookup(env, type, name, signature, 1);
}

JNIEXPORT jstring JNICALL PREFIX(instanceLookup)(JNIEnv *env, jclass self, jclass type, jstring name,
                                                  jstring signature) {
    (void)self;
    return lookup(env, type, name, signature, 0);
}

// The static field a reflected Field names, read through the ID FromReflectedField gives, as its toString.
JNIEXPORT jstring JNICALL PREFIX(fromReflected)(JNIEnv *env, jclass self, jobject field) {
    jfieldID id = (*env)->FromReflectedField(env, field);
    char line[512];
    if (id == NULL) {
        take_exception(env, line, sizeof line);
        return (*env)->NewStringUTF(env, line);
    }
    jobject value = (*env)->GetStaticObjectField(env, self, id);
    if (value == NULL) return (*env)->NewStringUTF(env, "an ID, the value null");
    jmethodID to_string = (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/Object"), "toString",
                                              "()Ljava/lang/String;");
    jstring text = (jstring)(*env)->CallObjectMethod(env, value, to_string);
    const char *chars = (*env)->GetStringUTFChars(env, text, NULL);
    snprintf(line, sizeof line, "an ID, the value %s", chars);
    (*env)->ReleaseStringUTFChars(env, text, chars);
    return (*env)->NewStringUTF(env, line);
}

// The Field ToReflectedField makes of a looked-up ID: its toString.
JNIEXPORT jstring JNICALL PREFIX(toReflected)(JNIEnv *env, jclass self, jclass type, jstring name, jstring signature,
                                              jboolean is_static) {
    (void)self;
    const char *name_text = (*env)->GetStringUTFChars(env, name, NULL);
    const char *signature_text = (*env)->GetStringUTFChars(env, signature, NULL);
    jfieldID id = is_static ? (*env)->GetStaticFieldID(env, type, name_text, signature_text)
                            : (*env)->GetFieldID(env, type, name_text, signature_text);
    (*env)->ReleaseStringUTFChars(env, name, name_text);
    (*env)->ReleaseStringUTFChars(env, signature, signature_text);
    char line[512];
    if (id == NULL) {
        take_exception(env, line, sizeof line);
        return (*env)->NewStringUTF(env, line);
    }
    jobject field = (*env)->ToReflectedField(env, type, id, is_static);
    if (field == NULL) {
        take_exception(env, line, sizeof line);
        return (*env)->NewStringUTF(env, line);
    }
    jmethodID to_string = (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/Object"), "toString",
                                              "()Ljava/lang/String;");
    return (jstring)(*env)->CallObjectMethod(env, field, to_string);
}

static void find_class(JNIEnv *env, const char *name, char *out, size_t size) {
    jclass found = (*env)->FindClass(env, name);
    if (found != NULL) {
        snprintf(out, size, "found");
    } else {
        take_exception(env, out, size);
    }
}

JNIEXPORT jstring JNICALL PREFIX(findClassHere)(JNIEnv *env, jclass self, jstring name) {
    (void)self;
    const char *name_text = (*env)->GetStringUTFChars(env, name, NULL);
    char line[512];
    find_class(env, name_text, line, sizeof line);
    (*env)->ReleaseStringUTFChars(env, name, name_text);
    return (*env)->NewStringUTF(env, line);
}

struct attached {
    const char *name;
    char line[512];
};

static void *on_attached_thread(void *argument) {
    struct attached *work = argument;
    JNIEnv *env = NULL;
    if ((*the_vm)->AttachCurrentThread(the_vm, &env, NULL) != JNI_OK) {
        snprintf(work->line, sizeof work->line, "AttachCurrentThread failed");
        return NULL;
    }
    find_class(env, work->name, work->line, sizeof work->line);
    (*the_vm)->DetachCurrentThread(the_vm);
    return NULL;
}

JNIEXPORT jstring JNICALL PREFIX(findClassOnAttachedThread)(JNIEnv *env, jclass self, jstring name) {
    (void)self;
    struct attached work;
    work.name = (*env)->GetStringUTFChars(env, name, NULL);
    pthread_t thread;
    if (pthread_create(&thread, NULL, on_attached_thread, &work) != 0) {
        snprintf(work.line, sizeof work.line, "pthread_create failed");
    } else {
        pthread_join(thread, NULL);
    }
    (*env)->ReleaseStringUTFChars(env, name, work.name);
    return (*env)->NewStringUTF(env, work.line);
}
