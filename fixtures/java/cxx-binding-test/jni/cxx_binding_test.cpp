// The self-quit crash's test (cxx-binding-test): a JNI library with the NDK's static C++ library, as Unity's
// libil2cpp.so has one. Its preemptible references to the C++ runtime (the personality routine that its unwind tables
// name, __cxa_throw, __cxa_begin_catch, operator new, std::terminate) go through its GOT, so the dynamic linker binds
// them: to the library's own copy on Android, unless an image ahead of it in the global group exports one. A copy of
// another image's personality reads the library's unwind context with another unwinder. Then a C++ exception thrown
// and caught on a thread of its own.
#include <dlfcn.h>
#include <jni.h>

#include <cstddef>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" void __gxx_personality_v0();
extern "C" void __cxa_throw();
extern "C" void __cxa_begin_catch();

namespace {

std::string own_image;

void mark() {}

// The image that holds `address`: "the library itself", or its file name.
std::string image_of(const void *address) {
    Dl_info info{};
    if (dladdr(address, &info) == 0 || info.dli_fname == nullptr) return "no image";
    std::string name = info.dli_fname;
    if (name == own_image) return "the library itself";
    const auto slash = name.rfind('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL Java_io_github_kisame76_minato_test_cxxbinding_CxxBindingApplication_bindings(
    JNIEnv *env, jclass) {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&mark), &info) != 0 && info.dli_fname != nullptr) {
        own_image = info.dli_fname;
    }
    void *(*allocate)(std::size_t) = &::operator new;
    std::string text = "__gxx_personality_v0: " + image_of(reinterpret_cast<const void *>(&__gxx_personality_v0)) +
                       "; __cxa_throw: " + image_of(reinterpret_cast<const void *>(&__cxa_throw)) +
                       "; __cxa_begin_catch: " + image_of(reinterpret_cast<const void *>(&__cxa_begin_catch)) +
                       "; operator new: " + image_of(reinterpret_cast<const void *>(allocate)) +
                       "; std::terminate: " + image_of(reinterpret_cast<const void *>(&std::terminate));
    return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_io_github_kisame76_minato_test_cxxbinding_CxxBindingApplication_throwOnThread(
    JNIEnv *env, jclass) {
    std::string outcome = "nothing caught";
    std::thread thread([&outcome] {
        try {
            throw std::runtime_error("thrown on a thread");
        } catch (const std::exception &caught) {
            outcome = std::string("caught: ") + caught.what();
        }
    });
    thread.join();
    return env->NewStringUTF(outcome.c_str());
}
