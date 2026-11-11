// app_process (ADR-0039 decision 1): the guest program /system/bin/app_process64, which plays Android's app process
// for an app with Java code. Android's zygote forks it from app_process; Minato starts it directly, one app per
// process.
//   app_process64 [--nice-name=<process name>] --package <package> --apk <base.apk> --library-path <lib/arm64>
//                 --data-dir <data folder> [--application <class>]
// The process name is --nice-name's, as Android's app_process takes it, else argv[0]. It names the process as
// AndroidRuntime::setArgv0 does for an app: the argument block is overwritten with the name, so that
// /proc/self/cmdline, which reads the live block, shows it, and the main thread takes the name's last 15
// characters, as zygote's SetThreadName does, which also makes them liblog's default tag (base::SetDefaultTag), the
// tag of a LOG without LOG_TAG, as libandroidfw's. The arguments are copied first, for ActivityThread.main.
//
// As AndroidRuntime::startVm and dalvikvm do, it finds JNI_CreateJavaVM through libnativehelper's JniInvocation
// (libart.so) and creates the VM on the main thread:
//   - the boot class path from the environment's BOOTCLASSPATH, which ART reads when no -Xbootclasspath is given
//     (parsed_options.cc), and which the runtime sets as init does (decision 2), our framework.jar in it;
//   - the heap from the dalvik.vm.* properties as AndroidRuntime reads them, with the values of AOSP's
//     phone-xhdpi-6144-dalvik-heap.mk, its largest phone, where a property is missing (decision 4);
//   - the collector from dalvik.vm.gctype as AndroidRuntime reads it (-Xgc:), CMS where it is missing: short pauses
//     for an app (ADR-0033 decision 5, changed 2026-10-06). ART's own default stays SS, which the tests keep;
//   - ADR-0033 decision 11's options: nterp as the interpreter (no -Xint, M6), no boot image, no JDWP. No boot image is
//     -Xforcejitzygote, the option AndroidRuntime adds for the JIT zygote image. Checked at the tag:
//       - it sets -Ximage to GetJitZygoteBootImageLocation(), "/nonx/boot.art!…", which file_utils.cc calls
//         intentionally non-existing, so ART falls back to imageless running from the jars;
//       - GetBootImageLocationForDefaultBcp returns that same location for no_boot_image: ART's own "no boot
//         image" path;
//       - the boot class path is not compiled in memory: parsed_options.cc sets AllowInMemoryCompilation by
//         default only for -Xzygote, which app_process never passes.
//     Without it, ART derives the default image's location from BOOTCLASSPATH's mainline part after
//     DEX2OATBOOTCLASSPATH, which we do not have, and aborts ("No mainline framework library found"). M6's boot
//     image replaces it with a real -Ximage;
//   - -Xhidden-api-policy:just-warn: framework.jar and the boot jars carry Android 15's hidden-API flags (tools/
//     hiddenapi-encode.py), which ART reads; it denies nothing yet (the step after, "enabled", is Android's default)
//     and reports what an app reaches to StrictMode's detectNonSdkApiUsage. Without the flags' file (a clone that has
//     not fetched it) every member reads as public, as before.
// On the main thread right after the VM, as AndroidRuntime::start does, it registers framework.jar's natives with
// libandroid_runtime.so's register_framework_natives, our stand-in for the private AndroidRuntime::startReg
// (decision 10); their FindClass goes to the boot class path.
// Then it calls android.app.ActivityThread.main(String[]) with the arguments after argv[0] and the process name.
// Afterwards it goes on as AndroidRuntime::start does: "Shutting down VM", then the main thread's detach, which hands
// an exception that left main to the thread's uncaught exception handlers, as on Android, whose default ends the
// process with SIGKILL (ADR-0039, the main thread's uncaught exception).
// It ends with 0 when main returns, and with 1 and a line on stderr when the VM or the class fails, or when main
// threw and no handler ended the process.
//
// It links libsigchain ahead of libc, as Android's app_process and dalvikvm do (ADR-0033 decision 6), so that ART's
// signal handlers come first.
#include <android/log.h>
#include <register_framework_natives.h>
#include <jni.h>
#include <nativehelper/JniInvocation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/system_properties.h>

#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr const char *kDriver = "android/app/ActivityThread";

// A dalvik.vm.* property as an option, as AndroidRuntime's parseRuntimeOption makes it, or with `fallback`.
std::string property_option(const char *property, const char *prefix, const char *fallback) {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(property, value) <= 0) {
        snprintf(value, sizeof(value), "%s", fallback);
    }
    return std::string(prefix) + value;
}

// AndroidRuntime::setArgv0(name, true): the thread's name from the name's last 15 characters, which zygote's
// SetThreadName also makes liblog's default tag, then the whole argument block, from argv[0] to the last argument's
// NUL, cleared and given the name.
void set_argv0(int argc, char **argv, const char *name) {
    size_t length = strlen(name);
    const char *short_name = length < 15 ? name : name + length - 15;
    pthread_setname_np(pthread_self(), short_name);
    __android_log_set_default_tag(short_name);
    size_t block = static_cast<size_t>(argv[argc - 1] + strlen(argv[argc - 1]) + 1 - argv[0]);
    memset(argv[0], 0, block);
    strlcpy(argv[0], name, block);
}

int fail(const char *what) {
    fprintf(stderr, "app_process: %s\n", what);
    return EXIT_FAILURE;
}

}  // namespace

// Seconds since 1970 now, the clock the app's session check stamps its events with (SessionCheck.event), so that a
// start is read against them: how long the process took to reach main, to create the VM, to enter ActivityThread.
static double wall_seconds() {
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

int main(int argc, char **argv) {
    const double main_time = wall_seconds();
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 1) {
        return fail("no process name");
    }
    std::string process_name = argv[0];
    int first = 1;
    if (argc > 1 && strncmp(argv[1], "--nice-name=", strlen("--nice-name=")) == 0) {
        process_name = argv[1] + strlen("--nice-name=");
        first = 2;
    }
    const std::vector<std::string> driver_arguments(argv + first, argv + argc);
    set_argv0(argc, argv, process_name.c_str());

    // phone-xhdpi-6144-dalvik-heap.mk at the tag, where the property area has no value.
    std::vector<std::string> strings = {
        property_option("dalvik.vm.heapstartsize", "-Xms", "16m"),
        property_option("dalvik.vm.heapsize", "-Xmx", "512m"),
        property_option("dalvik.vm.heapgrowthlimit", "-XX:HeapGrowthLimit=", "256m"),
        property_option("dalvik.vm.heapminfree", "-XX:HeapMinFree=", "8m"),
        property_option("dalvik.vm.heapmaxfree", "-XX:HeapMaxFree=", "32m"),
        property_option("dalvik.vm.heaptargetutilization", "-XX:HeapTargetUtilization=", "0.5"),
        // ADR-0033 decision 5: CMS for an app, where the property area has no value.
        property_option("dalvik.vm.gctype", "-Xgc:", "CMS"),
        "-Xusejit:false",
        "-Xnoimage-dex2oat",
        "-Xforcejitzygote",
        "-XjdwpProvider:none",
        "-Xhidden-api-policy:just-warn",
    };
    // The Mac tests' measurement switch (M6 e0, tools/boot-start-measure.sh): MINATO_MEASURE_ART_OPTIONS adds options,
    // such as -Xverify:none to measure what verification costs, or -Xint. No app's launch sets it (GuestStart passes
    // only its own entries); -Xverify:none never becomes a default.
    if (const char *measured = getenv("MINATO_MEASURE_ART_OPTIONS")) {
        std::istringstream words(measured);
        for (std::string word; words >> word;) strings.push_back(word);
    }
    // nterp, ART's fast interpreter, is the default as on Android; -Xint would make ART force the switch interpreter
    // for every method (runtime.cc, Opt::Interpret) and so never use a compiled odex. The line tells the tests which.
    bool switch_interpreter = false;
    for (const std::string &option : strings) switch_interpreter = switch_interpreter || option == "-Xint";
    __android_log_print(ANDROID_LOG_INFO, "app_process", "interpreter: %s", switch_interpreter ? "switch" : "nterp");
    std::vector<JavaVMOption> options(strings.size());
    for (size_t i = 0; i < strings.size(); i++) {
        options[i].optionString = strings[i].c_str();
    }

    JniInvocation invocation;
    if (!invocation.Init(nullptr)) {
        return fail("cannot load the VM (libart.so)");
    }
    JavaVMInitArgs init_args = {};
    init_args.version = JNI_VERSION_1_6;
    init_args.options = options.data();
    init_args.nOptions = static_cast<jint>(options.size());
    init_args.ignoreUnrecognized = JNI_FALSE;
    JavaVM *vm = nullptr;
    JNIEnv *env = nullptr;
    if (JNI_CreateJavaVM(&vm, &env, &init_args) != JNI_OK) {
        return fail("cannot create the VM");
    }

    const double vm_time = wall_seconds();
    if (android::register_framework_natives(env) < 0) {
        return fail("cannot register the framework's natives");
    }
    jclass driver = env->FindClass(kDriver);
    if (driver == nullptr) {
        env->ExceptionDescribe();
        return fail("no android.app.ActivityThread in the boot class path");
    }
    jmethodID main_method = env->GetStaticMethodID(driver, "main", "([Ljava/lang/String;)V");
    if (main_method == nullptr) {
        env->ExceptionDescribe();
        return fail("no ActivityThread.main(String[])");
    }
    // The arguments after argv[0] and --nice-name, then the process name for the driver's currentProcessName.
    jclass string_class = env->FindClass("java/lang/String");
    const jsize count = static_cast<jsize>(driver_arguments.size());
    jobjectArray arguments = env->NewObjectArray(count + 2, string_class, nullptr);
    if (arguments == nullptr) {
        env->ExceptionDescribe();
        return fail("cannot pass the arguments");
    }
    for (jsize i = 0; i < count; i++) {
        env->SetObjectArrayElement(arguments, i, env->NewStringUTF(driver_arguments[i].c_str()));
    }
    env->SetObjectArrayElement(arguments, count, env->NewStringUTF("--process"));
    env->SetObjectArrayElement(arguments, count + 1, env->NewStringUTF(process_name.c_str()));

    __android_log_print(ANDROID_LOG_INFO, "app_process", "start-times main=%.3f vm=%.3f app=%.3f", main_time, vm_time,
                        wall_seconds());
    env->CallStaticVoidMethod(driver, main_method, arguments);
    // An exception that left main stays pending, as in AndroidRuntime::start, whose threadExitUncaughtException is
    // under #if 0 at the tag: the detach below hands it to the thread's uncaught exception handlers (ART's
    // Thread::HandleUncaughtExceptions), RuntimeInit's that ActivityThread.main installs or an app's that hands it on
    // to them, and their default logs FATAL EXCEPTION and kills the process. Returning instead ran exit's destructors
    // while the app's other threads still locked their mutexes. As dalvikvm, it checks first, as the detach clears it.
    const bool threw = env->ExceptionCheck();
    __android_log_print(ANDROID_LOG_DEBUG, "AndroidRuntime", "Shutting down VM\n");
    // As dalvikvm in a release build: detach, then let the VM go.
    if (vm->DetachCurrentThread() != JNI_OK) {
        return fail("cannot detach the main thread");
    }
    if (vm->DestroyJavaVM() != JNI_OK) {
        return fail("the VM did not shut down cleanly");
    }
    // An app's handler that kept the process: it ends once its other threads have, with dalvikvm's status.
    return threw ? fail("ActivityThread.main ended with an exception that no handler ended the process for")
                 : EXIT_SUCCESS;
}
