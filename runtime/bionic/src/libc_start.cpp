// Minato, added to bionic's libc.so (ADR-0021): sets up the main thread of a dynamic program the
// way linker64 does before any constructor runs (linker_main.cpp, linker_tls.cpp): the thread
// pointer, the main thread's pthread_internal_t and bionic_tls, the stack guard, the start
// environment, and the static TLS of the program and its libraries. The runtime's loader calls it on
// the program thread in place of linker64. Built without the stack protector, like the rest of
// libc_bootstrap: the stack guard lives behind the thread pointer, which is set up here.
#include "APKRuntime/guest_interface.h"

#include <unistd.h>

#include "bionic/pthread_internal.h"
#include "private/KernelArgumentBlock.h"
#include "private/ScopedRWLock.h"
#include "private/ScopedSignalBlocker.h"
#include "private/bionic_elf_tls.h"
#include "private/bionic_globals.h"

// linker64's TLS descriptor resolvers (linker/arch/arm64/tlsdesc_resolver.S), built into libc.so.
extern "C" void tlsdesc_resolver_static();
extern "C" void tlsdesc_resolver_dynamic();
extern "C" void tlsdesc_resolver_unresolved_weak();

extern "C" void __libc_init_main_thread_early(const KernelArgumentBlock& args, bionic_tcb* temp_tcb);
extern "C" void __libc_init_main_thread_late();
extern "C" void __libc_init_main_thread_final();

namespace {

// linker64 keeps its TLS modules in a std::vector that grows; here the table has a fixed size, so
// that it never moves while other threads read it. The objects loaded at start come first, with
// static TLS; libraries dlopen loads later get dynamic TLS.
constexpr size_t kMaxModules = 1024;
TlsModule modules_table[kMaxModules];

}  // namespace

// For the runtime's loader, which fills in TLS descriptors as linker64 does (guest_interface.h), in
// the order of enum apkrt_tlsdesc_resolver.
extern "C" __attribute__((visibility("default"))) void* const __apkrt_tlsdesc_resolvers[APKRT_TLSDESC_RESOLVER_COUNT] = {
    reinterpret_cast<void*>(tlsdesc_resolver_static),
    reinterpret_cast<void*>(tlsdesc_resolver_dynamic),
    reinterpret_cast<void*>(tlsdesc_resolver_unresolved_weak),
};
static_assert(APKRT_TLSDESC_STATIC == 0 && APKRT_TLSDESC_DYNAMIC == 1 && APKRT_TLSDESC_UNRESOLVED_WEAK == 2);

extern "C" __attribute__((visibility("default"))) int __apkrt_start_main_thread(apkrt_start* start) {
  KernelArgumentBlock args(start->start_stack);
  bionic_tcb temp_tcb = {};
  __libc_init_main_thread_early(args, &temp_tcb);
  __libc_init_main_thread_late();
  __libc_shared_globals()->init_progname = args.argv[0];
  // The start environment, as linker64 hands it over: linker_main.cpp calls __libc_init_AT_SECURE(args.envp),
  // which ends with `environ = __libc_shared_globals()->init_environ = env;` (libc_init_common.cpp), and
  // __libc_init_common sets environ from init_environ again. There are no setuid programs, so no AT_SECURE
  // and nothing to sanitize.
  environ = __libc_shared_globals()->init_environ = args.envp;

  // linker_setup_exe_static_tls, register_soinfo_tls and linker_finalize_static_tls.
  StaticTlsLayout& layout = __libc_shared_globals()->static_tls_layout;
  TlsModules& modules = __libc_shared_globals()->tls_modules;
  size_t count = 0;
  for (size_t index = 0; index < start->object_count; ++index) {
    apkrt_object& object = start->objects[index];
    TlsSegment segment;
    const bool has_tls = __bionic_get_tls_segment(static_cast<const ElfW(Phdr)*>(object.phdr), object.phnum,
                                                  object.bias, &segment);
    size_t offset;
    if (index == 0) {
      offset = layout.reserve_exe_segment_and_tcb(has_tls ? &segment : nullptr, args.argv[0]);
      layout.reserve_bionic_tls();
    } else if (has_tls) {
      offset = layout.reserve_solib_segment(segment);
    }
    if (!has_tls) continue;
    if (count == kMaxModules) return -1;
    const size_t generation = ++modules.generation;
    __libc_tls_generation_copy = generation;
    modules_table[count] = TlsModule{
        .segment = segment,
        .static_offset = offset,
        .first_generation = generation,
        .soinfo_ptr = &object,
    };
    object.tls_module_id = __tls_module_idx_to_id(count);
    ++count;
  }
  modules.module_table = modules_table;
  modules.module_count = count;
  layout.finish_layout();
  modules.static_module_count = count;

  __libc_init_main_thread_final();

  for (size_t index = 0; index < count; ++index) {
    auto* object = static_cast<apkrt_object*>(modules_table[index].soinfo_ptr);
    object->tls_offset = static_cast<int64_t>(modules_table[index].static_offset) -
                         static_cast<int64_t>(layout.offset_thread_pointer());
  }
  return 0;
}

// linker64's register_soinfo_tls for a library dlopen loaded: a dynamic TLS module, whose memory
// each thread gets at its first access (__tls_get_addr).
extern "C" __attribute__((visibility("default"))) uint64_t __apkrt_register_tls_module(apkrt_object* object,
                                                                                    uint64_t* generation) {
  TlsSegment segment;
  if (!__bionic_get_tls_segment(static_cast<const ElfW(Phdr)*>(object->phdr), object->phnum, object->bias,
                                &segment)) {
    return 0;
  }
  TlsModules& modules = __libc_shared_globals()->tls_modules;
  ScopedSignalBlocker ssb;
  ScopedWriteLock locker(&modules.rwlock);
  if (modules.module_count == kMaxModules) return 0;
  const size_t index = modules.module_count;
  const size_t new_generation = ++modules.generation;
  __libc_tls_generation_copy = new_generation;
  modules_table[index] = TlsModule{
      .segment = segment,
      .static_offset = SIZE_MAX,
      .first_generation = new_generation,
      .soinfo_ptr = object,
  };
  modules.module_count = index + 1;
  object->tls_module_id = __tls_module_idx_to_id(index);
  *generation = new_generation;
  return object->tls_module_id;
}
