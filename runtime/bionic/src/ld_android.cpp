// Minato's ld-android.so (ADR-0021), in place of bionic's stub: what linker64 provides to libc.so
// and libdl.so in-process, on top of the runtime's loader.
//   - libc_shared_globals lives here, as in linker64. It is constant-initialized, so libc.so can use
//     it before any constructor has run.
//   - dl_iterate_phdr walks the objects the runtime loaded (apkrt_loader.objects); the unwinder
//     finds .eh_frame through it. Their TLS blocks are found as linker64 finds them.
//   - dlopen, dlsym, dladdr and friends ask the runtime through apkrt_loader, which it fills in
//     before any guest code runs. Namespaces are names for the loader's one scope with folders of
//     their own (ADR-0033 decision 7, until step e).
//   - They take a lock of their own where linker64 takes g_dl_mutex, so that a thread waits for
//     another one's dlopen here, in guest code, where signals reach it (ADR-0025, decision 8).
// Built without the stack protector: libc.so calls in before the thread pointer is set up.
#include "APKRuntime/guest_interface.h"

#include <android/dlext.h>
#include <asm/unistd.h>
#include <dlfcn.h>
#include <link.h>
#include <linux/futex.h>
#include <signal.h>
#include <string.h>

#include "bionic/pthread_internal.h"
#include "private/bionic_elf_tls.h"
#include "private/bionic_globals.h"

extern "C" {

// What linker64 reports for a program without an app around it: __ANDROID_API__ of the platform
// build, __ANDROID_API_FUTURE__. An app's runtime sets the app's target SDK (M4).
__attribute__((visibility("default"))) apkrt_loader apkrt_loader = {
    .target_sdk_version = __ANDROID_API_FUTURE__,
    .objects = nullptr,
    .object_count = 0,
    .dlopen = nullptr,
    .dlsym = nullptr,
    .dlclose = nullptr,
    .dlerror = nullptr,
    .dladdr = nullptr,
    .dlopen_in = nullptr,
    .create_namespace = nullptr,
    .link_namespaces = nullptr,
    .exported_namespace = nullptr,
    .dlopen_namespace = nullptr,
};

namespace {

libc_shared_globals shared_globals;
const char* no_loader = "the runtime offers no dynamic loading here";
const char* pending_error;

// A futex call through svc #0: ld-android.so has no libc, because libc.so needs it. The converter
// routes the svc to the runtime like every other guest syscall.
long futex(int* word, int operation, int value) {
  register long x0 __asm__("x0") = reinterpret_cast<long>(word);
  register long x1 __asm__("x1") = operation;
  register long x2 __asm__("x2") = value;
  register long x3 __asm__("x3") = 0;  // no timeout
  register long x8 __asm__("x8") = __NR_futex;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}

// linker64's g_dl_mutex, which is recursive: a constructor that dlopen runs may call libdl again.
// The word is the futex mutex of Drepper's "Futexes Are Tricky": 0 free, 1 held, 2 held with
// waiters. As in bionic's recursive mutex, a handler that calls libdl while its thread is between
// taking the word and writing the owner waits for itself.
class LoaderLock {
 public:
  void lock() {
    void* self = __builtin_thread_pointer();
    if (__atomic_load_n(&owner_, __ATOMIC_RELAXED) == self) {  // only this thread stores self
      ++depth_;
      return;
    }
    int state = 0;
    if (!__atomic_compare_exchange_n(&word_, &state, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      while (__atomic_exchange_n(&word_, 2, __ATOMIC_ACQUIRE) != 0) futex(&word_, FUTEX_WAIT_PRIVATE, 2);
    }
    __atomic_store_n(&owner_, self, __ATOMIC_RELAXED);
    depth_ = 1;
  }

  void unlock() {
    if (--depth_ != 0) return;
    __atomic_store_n(&owner_, nullptr, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&word_, 0, __ATOMIC_RELEASE) == 2) futex(&word_, FUTEX_WAKE_PRIVATE, 1);
  }

 private:
  int word_ = 0;
  void* owner_ = nullptr;
  int depth_ = 0;
};

LoaderLock loader_lock;

class Locked {
 public:
  Locked() { loader_lock.lock(); }
  ~Locked() { loader_lock.unlock(); }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;
};

}  // namespace

libc_shared_globals* __loader_shared_globals() {
  return &shared_globals;
}

void* __loader_dlopen(const char* filename, int flags, const void* caller) {
  Locked locked;
  if (apkrt_loader.dlopen == nullptr) {
    pending_error = no_loader;
    return nullptr;
  }
  int error_number = 0;
  void* handle = apkrt_loader.dlopen(filename, flags, caller, &error_number);
  // What linker64's failed open() calls leave in errno. libc.so's __errno() is out of reach here,
  // so straight into the thread's pthread_internal_t, where it keeps errno.
  if (error_number != 0) __get_thread()->errno_value = error_number;
  return handle;
}

namespace {

// Namespaces as ADR-0033 decision 7 has them until step e (ADR-0040): every namespace is the loader's one
// global scope, and a namespace only names the folders where android_dlopen_ext looks for a library by name
// first. ART's start loads its JNI libraries into the exported namespace com_android_art, and libnativeloader
// opens a boot jar's JNI library in its APEX's exported namespace, conscrypt.jar's in com_android_conscrypt. It
// asks for the exported "system" or "default" namespace and creates one for each class loader.
struct Namespace {
  char name[64];
  char search_path[1024];  // folders, separated by colons
};

Namespace default_namespace = {"default", ""};
Namespace art_namespace = {"com_android_art", "/apex/com.android.art/lib64"};
Namespace conscrypt_namespace = {"com_android_conscrypt", "/apex/com.android.conscrypt/lib64"};
// Room for the namespaces of class loaders: ld-android.so has no allocator. Guarded by loader_lock.
Namespace created_namespaces[32];
size_t created_count = 0;

bool same(const char* a, const char* b) {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// Appends `text` to the string in `buffer`, as far as it fits; false when it does not fit whole.
bool append(char* buffer, size_t size, const char* text) {
  size_t at = 0;
  while (at < size && buffer[at] != '\0') ++at;
  for (; *text != '\0'; ++text) {
    if (at + 1 >= size) return false;
    buffer[at++] = *text;
  }
  buffer[at] = '\0';
  return true;
}

}  // namespace

void* __loader_android_dlopen_ext(const char* filename, int flags, const android_dlextinfo* extinfo,
                                  const void* caller) {
  // File descriptors and reserved addresses have no meaning for the runtime's loader. ANDROID_DLEXT_FORCE_LOAD, which
  // ART passes for every oat file (oat_file.cc), is a plain load: dyld keeps one image per container path, so the
  // runtime has no second copy to give for it (ADR-0053, M6 b); a distinct image is c)'s, with a copy of the file).
  if (extinfo != nullptr && (extinfo->flags & ~(ANDROID_DLEXT_USE_NAMESPACE | ANDROID_DLEXT_FORCE_LOAD)) != 0) {
    pending_error = "android_dlopen_ext: extended options are not supported";
    return nullptr;
  }
  if (extinfo != nullptr && (extinfo->flags & ANDROID_DLEXT_USE_NAMESPACE) != 0 &&
      extinfo->library_namespace != nullptr && filename != nullptr) {
    {
      // The runtime's namespace, whose handle this is (ADR-0033 e2a), as linker64 loads into a namespace.
      Locked locked;
      if (apkrt_loader.dlopen_namespace != nullptr) {
        int error_number = 0;
        void* handle = apkrt_loader.dlopen_namespace(filename, flags, caller, &error_number,
                                                     extinfo->library_namespace);
        if (error_number != 0) __get_thread()->errno_value = error_number;
        return handle;
      }
    }
    const auto* space = reinterpret_cast<const Namespace*>(extinfo->library_namespace);
    {
      // An e0 runtime searches the namespace's folders for the library, for the libraries loaded for it and for
      // their dlopen by name, as linker64 searches the namespace a library belongs to (ADR-0033 e0).
      Locked locked;
      if (apkrt_loader.dlopen_in != nullptr) {
        int error_number = 0;
        void* handle = apkrt_loader.dlopen_in(filename, flags, caller, &error_number, space->search_path);
        if (error_number != 0) __get_thread()->errno_value = error_number;
        return handle;
      }
    }
    // A runtime without dlopen_in: the namespace's folders for a plain name, then wherever a plain dlopen looks.
    bool plain_name = true;
    for (const char* at = filename; *at != '\0'; ++at) plain_name = plain_name && *at != '/';
    for (const char* folder = space->search_path; plain_name && *folder != '\0';) {
      // Not zeroed as a whole, -ftrivial-auto-var-init or not: ld-android.so has no memset.
      [[clang::uninitialized]] char path[1024];
      size_t length = 0;
      while (folder[length] != '\0' && folder[length] != ':' && length + 1 < sizeof path) {
        path[length] = folder[length];
        ++length;
      }
      path[length] = '\0';
      folder += length;
      if (*folder == ':') ++folder;
      if (length != 0 && append(path, sizeof path, "/") && append(path, sizeof path, filename)) {
        if (void* handle = __loader_dlopen(path, flags, caller)) return handle;
      }
    }
  }
  return __loader_dlopen(filename, flags, caller);
}

void* __loader_dlsym(void* handle, const char* symbol, const void* caller) {
  Locked locked;
  if (apkrt_loader.dlsym == nullptr) {
    pending_error = no_loader;
    return nullptr;
  }
  return apkrt_loader.dlsym(handle, symbol, nullptr, caller);
}

void* __loader_dlvsym(void* handle, const char* symbol, const char* version, const void* caller) {
  Locked locked;
  if (apkrt_loader.dlsym == nullptr) {
    pending_error = no_loader;
    return nullptr;
  }
  return apkrt_loader.dlsym(handle, symbol, version, caller);
}

int __loader_dlclose(void* handle) {
  Locked locked;
  return apkrt_loader.dlclose == nullptr ? 0 : apkrt_loader.dlclose(handle);
}

char* __loader_dlerror() {
  const char* error = pending_error;
  pending_error = nullptr;
  if (error == nullptr && apkrt_loader.dlerror != nullptr) error = apkrt_loader.dlerror();
  return const_cast<char*>(error);
}

int __loader_dladdr(const void* address, Dl_info* info) {
  Locked locked;
  return apkrt_loader.dladdr == nullptr ? 0 : apkrt_loader.dladdr(address, info);
}

namespace {

// linker64's get_tls_block_for_this_thread without allocating: a static module's block lies at its
// offset from the thread pointer; a dynamic module has a block only once this thread touched it.
void* tls_block(const apkrt_object& object) {
  if (object.tls_module_id == 0) return nullptr;
  const TlsModules& modules = shared_globals.tls_modules;
  const size_t index = __tls_module_id_to_idx(object.tls_module_id);
  if (index >= modules.module_count) return nullptr;
  const TlsModule& module = modules.module_table[index];
  if (module.static_offset != SIZE_MAX) return static_cast<char*>(__builtin_thread_pointer()) + object.tls_offset;
  const TlsDtv* dtv = __get_tcb_dtv(__get_bionic_tcb());
  if (dtv->generation < module.first_generation || index >= dtv->count) return nullptr;
  return dtv->modules[index];
}

}  // namespace

int __loader_dl_iterate_phdr(int (*callback)(dl_phdr_info* info, size_t size, void* data), void* data) {
  Locked locked;  // also against a dlopen that registers a TLS module meanwhile
  const size_t count = __atomic_load_n(&apkrt_loader.object_count, __ATOMIC_ACQUIRE);
  for (size_t index = 0; index < count; ++index) {
    const apkrt_object& object = apkrt_loader.objects[index];
    dl_phdr_info info = {};
    info.dlpi_addr = object.bias;
    info.dlpi_name = object.name;
    info.dlpi_phdr = static_cast<const ElfW(Phdr)*>(object.phdr);
    info.dlpi_phnum = static_cast<ElfW(Half)>(object.phnum);
    info.dlpi_adds = count;
    info.dlpi_subs = 0;
    info.dlpi_tls_modid = object.tls_module_id;
    info.dlpi_tls_data = tls_block(object);
    if (const int result = callback(&info, sizeof info, data); result != 0) return result;
  }
  return 0;
}

void __loader_android_get_LD_LIBRARY_PATH(char* buffer, size_t buffer_size) {
  if (buffer_size > 0) buffer[0] = '\0';
}

void __loader_android_update_LD_LIBRARY_PATH(const char*) {}

void __loader_android_set_application_target_sdk_version(int target) {
  Locked locked;  // not in the middle of a dlopen, as in linker64
  apkrt_loader.target_sdk_version = target;
  if (shared_globals.set_target_sdk_version_hook != nullptr) shared_globals.set_target_sdk_version_hook(target);
}

int __loader_android_get_application_target_sdk_version() {
  return apkrt_loader.target_sdk_version;
}

bool __loader_android_init_anonymous_namespace(const char*, const char*) {
  return true;
}

android_namespace_t* __loader_android_create_namespace(const char* name, const char* ld_library_path,
                                                       const char* default_library_path, uint64_t type,
                                                       const char* permitted_path, android_namespace_t* parent,
                                                       const void* caller) {
  Locked locked;
  // The runtime's namespaces, as linker64's (ADR-0033 e2a); the minimal ones below on an older runtime.
  if (apkrt_loader.create_namespace != nullptr) {
    return reinterpret_cast<android_namespace_t*>(apkrt_loader.create_namespace(
        name, ld_library_path, default_library_path, type, permitted_path, parent, caller));
  }
  if (created_count == sizeof created_namespaces / sizeof created_namespaces[0]) {
    pending_error = "android_create_namespace: no room for another namespace";
    return nullptr;
  }
  Namespace& space = created_namespaces[created_count];
  space.name[0] = '\0';
  space.search_path[0] = '\0';
  append(space.name, sizeof space.name, name != nullptr ? name : "");
  // Its search path: LD_LIBRARY_PATH's part first, then the default library path, as linker64 searches them.
  const char* const parts[] = {ld_library_path, default_library_path};
  for (const char* part : parts) {
    if (part == nullptr || *part == '\0') continue;
    if ((space.search_path[0] != '\0' && !append(space.search_path, sizeof space.search_path, ":")) ||
        !append(space.search_path, sizeof space.search_path, part)) {
      pending_error = "android_create_namespace: the search path is too long";
      return nullptr;
    }
  }
  ++created_count;
  return reinterpret_cast<android_namespace_t*>(&space);
}

bool __loader_android_link_namespaces(android_namespace_t* from, android_namespace_t* to, const char* sonames) {
  Locked locked;
  if (apkrt_loader.link_namespaces == nullptr) return true;
  // linker64 refuses a link for no names; all of them is android_link_namespaces_all_libs.
  if (sonames == nullptr) sonames = "";
  return apkrt_loader.link_namespaces(from, to, sonames) == 0;
}

bool __loader_android_link_namespaces_all_libs(android_namespace_t* from, android_namespace_t* to) {
  Locked locked;
  return apkrt_loader.link_namespaces == nullptr || apkrt_loader.link_namespaces(from, to, nullptr) == 0;
}

// "default" stands for the program's namespace, as for a program in /system/bin (ADR-0040 decision 2),
// com_android_art for ART's APEX and com_android_conscrypt for Conscrypt's. libnativeloader ends the process
// when the namespace of a boot jar's APEX is missing (FindApexNamespace, ADR-0046 a).
android_namespace_t* __loader_android_get_exported_namespace(const char* name) {
  if (name == nullptr) return nullptr;
  {
    Locked locked;
    if (apkrt_loader.exported_namespace != nullptr) {
      return reinterpret_cast<android_namespace_t*>(apkrt_loader.exported_namespace(name));
    }
  }
  if (same(name, "default")) return reinterpret_cast<android_namespace_t*>(&default_namespace);
  if (same(name, "com_android_art")) return reinterpret_cast<android_namespace_t*>(&art_namespace);
  if (same(name, "com_android_conscrypt")) return reinterpret_cast<android_namespace_t*>(&conscrypt_namespace);
  return nullptr;
}

void __loader_android_dlwarning(void*, void (*)(void*, const char*)) {}

// ld-android.so needs nothing from libc.so, which needs it: a CFI failure traps.
void __loader_cfi_fail(uint64_t, void*, void*, void*) {
  __builtin_trap();
}

// Loaded code is never unloaded (ADR-0003), so nothing to keep alive for thread_local destructors.
void __loader_add_thread_local_dtor(void*) {}
void __loader_remove_thread_local_dtor(void*) {}

bool __loader_android_handle_signal(int, siginfo_t*, void*) {
  return false;
}

// A debugger sets a breakpoint here to hear about loaded libraries.
void rtld_db_dlactivity() {}

}  // extern "C"
