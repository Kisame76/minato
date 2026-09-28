// jni-test: activity-host's JNI stub (ADR-0028, decision 8), built from runtime/activity-host/jni_stub.cpp:
// every slot of both tables is a function; GetVersion, ExceptionCheck and GetJavaVM answer as ART does;
// every other function logs its name the first time and answers 0, NULL or false, JNI_ERR where only
// failing makes sense, whatever its result's kind; FindClass logs each class name once; the JavaVM
// attaches and detaches threads, each with a JNIEnv of its own, the first thread attached from the start.
// activity-test compares with Android 15 what an app sees of it. A test of the runtime: Android's JNI is
// ART's.
// Expected output: fixtures/expected/jni-test.out and .err
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "jni_stub.h"

static JavaVM *vm;
static JNIEnv *main_env;

static void *other_thread(void *unused) {
    (void)unused;
    JNIEnv *env = (JNIEnv *)1, *attached = NULL;
    const jint detached = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    printf("other thread: GetEnv %d (%s)", detached, env == NULL ? "NULL" : "set");
    const jint attach = (*vm)->AttachCurrentThreadAsDaemon(vm, &attached, NULL);
    const jint get = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    printf(", AttachCurrentThreadAsDaemon %d, GetEnv %d (%s, %s)", attach, get,
           env == attached ? "the attached env" : "another env",
           attached != NULL && attached != main_env ? "not the main thread's" : "the main thread's");
    const jint detach = (*vm)->DetachCurrentThread(vm);
    const jint detach_again = (*vm)->DetachCurrentThread(vm);
    printf(", DetachCurrentThread %d, again %d, GetEnv %d\n", detach, detach_again,
           (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6));
    return NULL;
}

int main(void) {
    jobject clazz = NULL;
    minato_jni_stub(&vm, &main_env, &clazz);
    if (vm == NULL || main_env == NULL) {
        printf("no stub\n");
        return 1;
    }
    JNIEnv *env = main_env;
    const struct JNINativeInterface *functions = *env;
    void *const *slots = (void *const *)functions;
    size_t filled = 0, count = sizeof *functions / sizeof(void *);
    for (size_t index = 4; index < count; ++index) filled += slots[index] != NULL;
    void *const *invoke = (void *const *)*vm;
    size_t invoke_filled = 0, invoke_count = sizeof **vm / sizeof(void *);
    for (size_t index = 3; index < invoke_count; ++index) invoke_filled += invoke[index] != NULL;
    printf("functions: %zu of %zu, invoke: %zu of %zu, clazz %s\n", filled, count - 4, invoke_filled, invoke_count - 3,
           clazz != NULL ? "set" : "NULL");

    printf("GetVersion 0x%x, ExceptionCheck %d\n", (*env)->GetVersion(env), (*env)->ExceptionCheck(env));
    jclass string = (*env)->FindClass(env, "java/lang/String");
    jclass again = (*env)->FindClass(env, "java/lang/String");
    jclass activity = (*env)->FindClass(env, "android/app/Activity");
    printf("FindClass: %s, %s, %s\n", string ? "set" : "NULL", again ? "set" : "NULL", activity ? "set" : "NULL");
    // One call per statement: each logs once, and the log's order is the calls' order.
    const float float_field = (*env)->GetFloatField(env, NULL, NULL);
    const double double_field = (*env)->GetDoubleField(env, NULL, NULL);
    const float float_method = (*env)->CallStaticFloatMethod(env, NULL, NULL, 1, 2.0);
    const float float_again = (*env)->GetFloatField(env, NULL, NULL);
    printf("GetFloatField %g, GetDoubleField %g, CallStaticFloatMethod %g, GetFloatField again %g\n",
           (double)float_field, double_field, (double)float_method, (double)float_again);
    const jstring text = (*env)->NewStringUTF(env, "text");
    const char *chars = (*env)->GetStringUTFChars(env, NULL, NULL);
    const jclass class_of = (*env)->GetObjectClass(env, clazz);
    printf("NewStringUTF %s, GetStringUTFChars %s, GetObjectClass %s\n", text ? "set" : "NULL", chars ? "set" : "NULL",
           class_of ? "set" : "NULL");
    const jint register_natives = (*env)->RegisterNatives(env, NULL, NULL, 0);
    const jint throw_object = (*env)->Throw(env, NULL);
    const jint throw_new = (*env)->ThrowNew(env, NULL, "x");
    const jint enter = (*env)->MonitorEnter(env, NULL);
    const jint exit_monitor = (*env)->MonitorExit(env, NULL);
    const jint unregister = (*env)->UnregisterNatives(env, NULL);
    printf("RegisterNatives %d, Throw %d, ThrowNew %d, MonitorEnter %d, MonitorExit %d, UnregisterNatives %d\n",
           register_natives, throw_object, throw_new, enter, exit_monitor, unregister);
    const jsize length = (*env)->GetArrayLength(env, NULL);
    const jint frame = (*env)->PushLocalFrame(env, 16);
    const jint capacity = (*env)->EnsureLocalCapacity(env, 16);
    const jboolean same = (*env)->IsSameObject(env, NULL, NULL);
    const jlong long_field = (*env)->GetLongField(env, NULL, NULL);
    const jlong buffer_capacity = (*env)->GetDirectBufferCapacity(env, NULL);
    printf("GetArrayLength %d, PushLocalFrame %d, EnsureLocalCapacity %d, IsSameObject %d, GetLongField %lld, "
           "GetDirectBufferCapacity %lld\n",
           length, frame, capacity, same, (long long)long_field, (long long)buffer_capacity);
    (*env)->CallVoidMethod(env, clazz, NULL);
    (*env)->ExceptionClear(env);
    JavaVM *from_env = NULL;
    const jint java_vm = (*env)->GetJavaVM(env, &from_env);
    printf("GetJavaVM %d (%s)\n", java_vm, from_env == vm ? "the stub's" : "another");

    JNIEnv *got = NULL, *attached = NULL;
    const jint get = (*vm)->GetEnv(vm, (void **)&got, JNI_VERSION_1_6);
    printf("main thread: GetEnv %d (%s)", get, got == main_env ? "its env" : "another env");
    const jint unknown = (*vm)->GetEnv(vm, (void **)&got, 0x7fffffff);
    const jint v11 = (*vm)->GetEnv(vm, (void **)&got, JNI_VERSION_1_1);
    const jint v12 = (*vm)->GetEnv(vm, (void **)&got, JNI_VERSION_1_2);
    const jint v14 = (*vm)->GetEnv(vm, (void **)&got, JNI_VERSION_1_4);
    printf(", version 0x7fffffff %d, 1.1 %d, 1.2 %d, 1.4 %d", unknown, v11, v12, v14);
    const jint attach = (*vm)->AttachCurrentThread(vm, &attached, NULL);
    printf(", AttachCurrentThread %d (%s)", attach, attached == main_env ? "its env" : "another env");
    printf(", DestroyJavaVM %d\n", (*vm)->DestroyJavaVM(vm));

    pthread_t thread;
    pthread_create(&thread, NULL, other_thread, NULL);
    pthread_join(thread, NULL);
    printf("main thread afterwards: GetEnv %d\n", (*vm)->GetEnv(vm, (void **)&got, JNI_VERSION_1_6));
    return 0;
}
