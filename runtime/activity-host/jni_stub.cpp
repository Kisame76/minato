// The JNI stub of activity-host (ADR-0028, decision 8): the JavaVM and the JNIEnv that ANativeActivity carries
// until ART's replace them in M4 (ADR-0033). The JavaVM attaches and detaches threads, each with a JNIEnv of
// its own; activity-host's main thread is attached from the start, as on Android. GetVersion, ExceptionCheck,
// GetJavaVM and FatalError (which logs the app's message and aborts) do as ART does. Every other function of
// the JNIEnv logs its name the first time and answers 0, NULL or false, whatever the kind of its result, or
// JNI_ERR where only failing makes sense; FindClass logs each class name once, which shows M4 what apps ask
// for. The answers of the JavaVM were compared with Android 15's by activity-test (fixtures/apps/activity-test).
#include "jni_stub.h"

#include <android/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr const char *kTag = "activity-host";

// Every function of JNINativeInterface, in the table's order (the NDK's jni.h).
#define JNI_FUNCTIONS(X) \
    X(GetVersion) X(DefineClass) X(FindClass) X(FromReflectedMethod) X(FromReflectedField) X(ToReflectedMethod) \
    X(GetSuperclass) X(IsAssignableFrom) X(ToReflectedField) X(Throw) X(ThrowNew) X(ExceptionOccurred) \
    X(ExceptionDescribe) X(ExceptionClear) X(FatalError) X(PushLocalFrame) X(PopLocalFrame) X(NewGlobalRef) \
    X(DeleteGlobalRef) X(DeleteLocalRef) X(IsSameObject) X(NewLocalRef) X(EnsureLocalCapacity) X(AllocObject) \
    X(NewObject) X(NewObjectV) X(NewObjectA) X(GetObjectClass) X(IsInstanceOf) X(GetMethodID) X(CallObjectMethod) \
    X(CallObjectMethodV) X(CallObjectMethodA) X(CallBooleanMethod) X(CallBooleanMethodV) X(CallBooleanMethodA) \
    X(CallByteMethod) X(CallByteMethodV) X(CallByteMethodA) X(CallCharMethod) X(CallCharMethodV) X(CallCharMethodA) \
    X(CallShortMethod) X(CallShortMethodV) X(CallShortMethodA) X(CallIntMethod) X(CallIntMethodV) X(CallIntMethodA) \
    X(CallLongMethod) X(CallLongMethodV) X(CallLongMethodA) X(CallFloatMethod) X(CallFloatMethodV) \
    X(CallFloatMethodA) X(CallDoubleMethod) X(CallDoubleMethodV) X(CallDoubleMethodA) X(CallVoidMethod) \
    X(CallVoidMethodV) X(CallVoidMethodA) X(CallNonvirtualObjectMethod) X(CallNonvirtualObjectMethodV) \
    X(CallNonvirtualObjectMethodA) X(CallNonvirtualBooleanMethod) X(CallNonvirtualBooleanMethodV) \
    X(CallNonvirtualBooleanMethodA) X(CallNonvirtualByteMethod) X(CallNonvirtualByteMethodV) \
    X(CallNonvirtualByteMethodA) X(CallNonvirtualCharMethod) X(CallNonvirtualCharMethodV) \
    X(CallNonvirtualCharMethodA) X(CallNonvirtualShortMethod) X(CallNonvirtualShortMethodV) \
    X(CallNonvirtualShortMethodA) X(CallNonvirtualIntMethod) X(CallNonvirtualIntMethodV) X(CallNonvirtualIntMethodA) \
    X(CallNonvirtualLongMethod) X(CallNonvirtualLongMethodV) X(CallNonvirtualLongMethodA) \
    X(CallNonvirtualFloatMethod) X(CallNonvirtualFloatMethodV) X(CallNonvirtualFloatMethodA) \
    X(CallNonvirtualDoubleMethod) X(CallNonvirtualDoubleMethodV) X(CallNonvirtualDoubleMethodA) \
    X(CallNonvirtualVoidMethod) X(CallNonvirtualVoidMethodV) X(CallNonvirtualVoidMethodA) X(GetFieldID) \
    X(GetObjectField) X(GetBooleanField) X(GetByteField) X(GetCharField) X(GetShortField) X(GetIntField) \
    X(GetLongField) X(GetFloatField) X(GetDoubleField) X(SetObjectField) X(SetBooleanField) X(SetByteField) \
    X(SetCharField) X(SetShortField) X(SetIntField) X(SetLongField) X(SetFloatField) X(SetDoubleField) \
    X(GetStaticMethodID) X(CallStaticObjectMethod) X(CallStaticObjectMethodV) X(CallStaticObjectMethodA) \
    X(CallStaticBooleanMethod) X(CallStaticBooleanMethodV) X(CallStaticBooleanMethodA) X(CallStaticByteMethod) \
    X(CallStaticByteMethodV) X(CallStaticByteMethodA) X(CallStaticCharMethod) X(CallStaticCharMethodV) \
    X(CallStaticCharMethodA) X(CallStaticShortMethod) X(CallStaticShortMethodV) X(CallStaticShortMethodA) \
    X(CallStaticIntMethod) X(CallStaticIntMethodV) X(CallStaticIntMethodA) X(CallStaticLongMethod) \
    X(CallStaticLongMethodV) X(CallStaticLongMethodA) X(CallStaticFloatMethod) X(CallStaticFloatMethodV) \
    X(CallStaticFloatMethodA) X(CallStaticDoubleMethod) X(CallStaticDoubleMethodV) X(CallStaticDoubleMethodA) \
    X(CallStaticVoidMethod) X(CallStaticVoidMethodV) X(CallStaticVoidMethodA) X(GetStaticFieldID) \
    X(GetStaticObjectField) X(GetStaticBooleanField) X(GetStaticByteField) X(GetStaticCharField) \
    X(GetStaticShortField) X(GetStaticIntField) X(GetStaticLongField) X(GetStaticFloatField) X(GetStaticDoubleField) \
    X(SetStaticObjectField) X(SetStaticBooleanField) X(SetStaticByteField) X(SetStaticCharField) \
    X(SetStaticShortField) X(SetStaticIntField) X(SetStaticLongField) X(SetStaticFloatField) X(SetStaticDoubleField) \
    X(NewString) X(GetStringLength) X(GetStringChars) X(ReleaseStringChars) X(NewStringUTF) X(GetStringUTFLength) \
    X(GetStringUTFChars) X(ReleaseStringUTFChars) X(GetArrayLength) X(NewObjectArray) X(GetObjectArrayElement) \
    X(SetObjectArrayElement) X(NewBooleanArray) X(NewByteArray) X(NewCharArray) X(NewShortArray) X(NewIntArray) \
    X(NewLongArray) X(NewFloatArray) X(NewDoubleArray) X(GetBooleanArrayElements) X(GetByteArrayElements) \
    X(GetCharArrayElements) X(GetShortArrayElements) X(GetIntArrayElements) X(GetLongArrayElements) \
    X(GetFloatArrayElements) X(GetDoubleArrayElements) X(ReleaseBooleanArrayElements) X(ReleaseByteArrayElements) \
    X(ReleaseCharArrayElements) X(ReleaseShortArrayElements) X(ReleaseIntArrayElements) X(ReleaseLongArrayElements) \
    X(ReleaseFloatArrayElements) X(ReleaseDoubleArrayElements) X(GetBooleanArrayRegion) X(GetByteArrayRegion) \
    X(GetCharArrayRegion) X(GetShortArrayRegion) X(GetIntArrayRegion) X(GetLongArrayRegion) X(GetFloatArrayRegion) \
    X(GetDoubleArrayRegion) X(SetBooleanArrayRegion) X(SetByteArrayRegion) X(SetCharArrayRegion) \
    X(SetShortArrayRegion) X(SetIntArrayRegion) X(SetLongArrayRegion) X(SetFloatArrayRegion) X(SetDoubleArrayRegion) \
    X(RegisterNatives) X(UnregisterNatives) X(MonitorEnter) X(MonitorExit) X(GetJavaVM) X(GetStringRegion) \
    X(GetStringUTFRegion) X(GetPrimitiveArrayCritical) X(ReleasePrimitiveArrayCritical) X(GetStringCritical) \
    X(ReleaseStringCritical) X(NewWeakGlobalRef) X(DeleteWeakGlobalRef) X(ExceptionCheck) X(NewDirectByteBuffer) \
    X(GetDirectBufferAddress) X(GetDirectBufferCapacity) X(GetObjectRefType)

enum Index {
#define X(name) index_##name,
    JNI_FUNCTIONS(X)
#undef X
    function_count
};
// With the table's four reserved slots, the list names every function.
static_assert(sizeof(JNINativeInterface) == (4 + function_count) * sizeof(void *));

constexpr const char *kNames[] = {
#define X(name) #name,
    JNI_FUNCTIONS(X)
#undef X
};

char logged[function_count];

void log_once(int index) {
    if (!__atomic_exchange_n(&logged[index], 1, __ATOMIC_RELAXED)) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "JNI %s: no Java before M4", kNames[index]);
    }
}

// A function with no answer of its own: it logs once and answers its result's zero (0, NULL, false, 0.0),
// in the register that kind of result goes to. Error stubs answer JNI_ERR.
template <int kIndex, typename Function>
struct Stub;
template <int kIndex, typename Result, typename... Arguments>
struct Stub<kIndex, Result (*)(Arguments...)> {
    static Result call(Arguments...) {
        log_once(kIndex);
        return Result();
    }
    static Result error(Arguments...) {
        log_once(kIndex);
        return JNI_ERR;
    }
};
template <int kIndex, typename Result, typename... Arguments>
struct Stub<kIndex, Result (*)(Arguments..., ...)> {
    static Result call(Arguments..., ...) {
        log_once(kIndex);
        return Result();
    }
};

jint get_version(JNIEnv *) {
    return JNI_VERSION_1_6;
}

jboolean exception_check(JNIEnv *) {
    return JNI_FALSE;
}

jlong direct_buffer_capacity(JNIEnv *, jobject) {
    log_once(index_GetDirectBufferCapacity);
    return -1;  // "not a direct buffer, or the VM has no JNI access to them"
}

// The class names FindClass has logged.
struct Logged {
    Logged *next;
    char name[];
};
Logged *logged_classes;
pthread_mutex_t classes_lock = PTHREAD_MUTEX_INITIALIZER;

jclass find_class(JNIEnv *, const char *name) {
    if (name == nullptr) name = "(null)";
    pthread_mutex_lock(&classes_lock);
    Logged *entry = logged_classes;
    while (entry != nullptr && strcmp(entry->name, name) != 0) entry = entry->next;
    if (entry == nullptr) {
        const size_t size = strlen(name) + 1;
        entry = static_cast<Logged *>(malloc(sizeof(Logged) + size));
        if (entry != nullptr) {
            memcpy(entry->name, name, size);
            entry->next = logged_classes;
            logged_classes = entry;
        }
        __android_log_print(ANDROID_LOG_WARN, kTag, "JNI FindClass(\"%s\"): no Java before M4", name);
    }
    pthread_mutex_unlock(&classes_lock);
    return nullptr;
}

// As ART's: the app's message goes to the log, and the program ends.
void fatal_error(JNIEnv *, const char *message) {
    __android_log_print(ANDROID_LOG_FATAL, kTag, "JNI FatalError called: %s", message != nullptr ? message : "(null)");
    abort();
}

jint get_java_vm(JNIEnv *, JavaVM **vm);

constexpr JNINativeInterface make_functions() {
    JNINativeInterface table{};
#define X(name) table.name = Stub<index_##name, decltype(table.name)>::call;
    JNI_FUNCTIONS(X)
#undef X
    table.GetVersion = get_version;
    table.ExceptionCheck = exception_check;
    table.FindClass = find_class;
    table.FatalError = fatal_error;
    table.GetJavaVM = get_java_vm;
    table.GetDirectBufferCapacity = direct_buffer_capacity;
    table.Throw = Stub<index_Throw, decltype(table.Throw)>::error;
    table.ThrowNew = Stub<index_ThrowNew, decltype(table.ThrowNew)>::error;
    table.RegisterNatives = Stub<index_RegisterNatives, decltype(table.RegisterNatives)>::error;
    table.UnregisterNatives = Stub<index_UnregisterNatives, decltype(table.UnregisterNatives)>::error;
    table.MonitorEnter = Stub<index_MonitorEnter, decltype(table.MonitorEnter)>::error;
    table.MonitorExit = Stub<index_MonitorExit, decltype(table.MonitorExit)>::error;
    return table;
}

constexpr JNINativeInterface kFunctions = make_functions();

// Each thread's JNIEnv, and whether the thread is attached.
thread_local _JNIEnv thread_env{&kFunctions};
thread_local bool attached;

bool known_version(jint version) {
    return version == JNI_VERSION_1_1 || version == JNI_VERSION_1_2 || version == JNI_VERSION_1_4 ||
           version == JNI_VERSION_1_6;
}

jint destroy_java_vm(JavaVM *) {
    return JNI_ERR;
}

jint attach_current_thread(JavaVM *, JNIEnv **env, void *) {
    attached = true;
    if (env != nullptr) *env = &thread_env;
    return JNI_OK;
}

jint detach_current_thread(JavaVM *) {
    if (!attached) return JNI_ERR;
    attached = false;
    return JNI_OK;
}

jint get_env(JavaVM *, void **env, jint version) {
    if (env == nullptr) return JNI_ERR;
    if (!attached) {
        *env = nullptr;
        return JNI_EDETACHED;
    }
    if (!known_version(version)) return JNI_EVERSION;
    *env = &thread_env;
    return JNI_OK;
}

constexpr JNIInvokeInterface kInvoke = {nullptr,       nullptr,         nullptr, destroy_java_vm, attach_current_thread,
                                        detach_current_thread, get_env, attach_current_thread};
_JavaVM java_vm{&kInvoke};

jint get_java_vm(JNIEnv *, JavaVM **vm) {
    if (vm == nullptr) return JNI_ERR;
    *vm = &java_vm;
    return JNI_OK;
}

// What ANativeActivity's clazz points at: a handle of the stub's own, never NULL.
char activity_object;

}  // namespace

void minato_jni_stub(JavaVM **vm, JNIEnv **env, jobject *clazz) {
    attach_current_thread(&java_vm, env, nullptr);
    *vm = &java_vm;
    *clazz = reinterpret_cast<jobject>(&activity_object);
}
