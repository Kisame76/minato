// app_process (ADR-0039 decision 1): the guest program /system/bin/app_process64, which plays Android's app process
// for an app with Java code. Android's zygote forks it from app_process; Minato starts it directly, one app per
// process (CLAUDE.md, decision 3).
//   app_process64 [--nice-name=<process name>] --package <package> --apk <base.apk> --library-path <lib/arm64>
//                 --data-dir <data folder> [--application <class>]
// The process name is --nice-name's, as Android's app_process takes it, else argv[0]. It names the process as
// AndroidRuntime::setArgv0 does for an app: the argument block is overwritten with the name, so that
// /proc/self/cmdline, which reads the live block, shows it, and the main thread takes the name's last 15
// characters, as zygote's SetThreadName does. The arguments are copied first, for ActivityThread.main.
//
// As AndroidRuntime::startVm and dalvikvm do, it finds JNI_CreateJavaVM through libnativehelper's JniInvocation
// (libart.so) and creates the VM on the main thread:
//   - the boot class path from the environment's BOOTCLASSPATH, which ART reads when no -Xbootclasspath is given
//     (parsed_options.cc), and which the runtime sets as init does (decision 2), our framework.jar in it;
//   - the heap from the dalvik.vm.* properties as AndroidRuntime reads them, with the values of AOSP's
//     phone-xhdpi-6144-dalvik-heap.mk, its largest phone, where a property is missing (decision 4);
//   - ADR-0033 decision 11's options: the interpreter only, no boot image, no JDWP. No boot image is
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
//   - -Xhidden-api-policy:disabled: our boot jars carry no hidden-API access flags, so ART enforces nothing either
//     way (decision 4).
// Then it calls android.app.ActivityThread.main(String[]) with the arguments after argv[0] and the process name.
// It ends with 0 when main returns, and with 1 and a line on stderr when the VM, the class or main fails.
//
// It links libsigchain ahead of libc, as Android's app_process and dalvikvm do (ADR-0033 decision 6), so that ART's
// signal handlers come first.
#include <jni.h>
#include <nativehelper/JniInvocation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/system_properties.h>

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

// AndroidRuntime::setArgv0(name, true): the thread's name from the name's last 15 characters, then the whole
// argument block, from argv[0] to the last argument's NUL, cleared and given the name.
void set_argv0(int argc, char **argv, const char *name) {
    size_t length = strlen(name);
    pthread_setname_np(pthread_self(), length < 15 ? name : name + length - 15);
    size_t block = static_cast<size_t>(argv[argc - 1] + strlen(argv[argc - 1]) + 1 - argv[0]);
    memset(argv[0], 0, block);
    strlcpy(argv[0], name, block);
}

int fail(const char *what) {
    fprintf(stderr, "app_process: %s\n", what);
    return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char **argv) {
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
        "-Xint",
        "-Xusejit:false",
        "-Xnoimage-dex2oat",
        "-Xforcejitzygote",
        "-XjdwpProvider:none",
        "-Xhidden-api-policy:disabled",
    };
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

    env->CallStaticVoidMethod(driver, main_method, arguments);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        return fail("ActivityThread.main ended with an exception");
    }
    // As dalvikvm in a release build: detach, then let the VM go.
    if (vm->DetachCurrentThread() != JNI_OK) {
        return fail("cannot detach the main thread");
    }
    if (vm->DestroyJavaVM() != JNI_OK) {
        return fail("the VM did not shut down cleanly");
    }
    return EXIT_SUCCESS;
}
