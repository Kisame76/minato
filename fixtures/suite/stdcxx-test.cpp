// stdcxx-test: Android's libstdc++.so, the NDK's "system" C++ runtime, which a
// library built with ANDROID_STL=system names in its NEEDED: new and delete in their plain, array and nothrow forms,
// the guard of a function's static, and the library each of these comes from.
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <new>

namespace {

int constructed = 0;

struct Counter {
    Counter() { ++constructed; }
};

Counter &counter() {
    static Counter instance;  // __cxa_guard_acquire and __cxa_guard_release around its construction
    return instance;
}

// The file name of the library that holds `address`, without its directory.
const char *library_of(const void *address) {
    Dl_info info{};
    if (address == nullptr || dladdr(address, &info) == 0 || info.dli_fname == nullptr) return "(none)";
    const char *slash = strrchr(info.dli_fname, '/');
    return slash != nullptr ? slash + 1 : info.dli_fname;
}

}  // namespace

int main() {
    auto *one = static_cast<int *>(::operator new(sizeof(int)));
    *one = 42;
    printf("operator new: %d\n", *one);
    ::operator delete(one);

    auto *many = static_cast<int *>(::operator new[](4 * sizeof(int)));
    for (int i = 0; i < 4; ++i) many[i] = i + 1;
    printf("operator new[]: %d %d %d %d\n", many[0], many[1], many[2], many[3]);
    ::operator delete[](many);

    void *small = ::operator new(16, std::nothrow);
    printf("nothrow new of 16 bytes: %s\n", small != nullptr ? "memory" : "null");
    ::operator delete(small, std::nothrow);

    counter();
    counter();
    printf("static constructed: %d time(s)\n", constructed);

    const auto plain_new = static_cast<void *(*)(size_t)>(&::operator new);
    printf("operator new from: %s\n", library_of(reinterpret_cast<const void *>(plain_new)));
    const char *const names[] = {"_ZdlPv", "_ZSt7nothrow", "__cxa_guard_acquire", "__cxa_pure_virtual"};
    for (const char *name : names) {
        printf("%s from: %s\n", name, library_of(dlsym(RTLD_DEFAULT, name)));
    }
    return 0;
}
