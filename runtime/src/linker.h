// The linker for dynamic programs (ADR-0021): reads PT_DYNAMIC of loaded containers, loads the
// libraries a program needs, resolves symbols in the global scope the way Android's linker64
// does, applies the symbolic relocations and runs the constructors in dependency order.
#pragma once

#include "loader.h"

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apkrt::linker {

/// Elf64_Sym.
struct Symbol {
    std::uint32_t name;
    std::uint8_t info;   // binding << 4 | type
    std::uint8_t other;  // visibility in the low 2 bits
    std::uint16_t section;
    std::uint64_t value;
    std::uint64_t size;
};
static_assert(sizeof(Symbol) == 24);

/// What the linker needs from an object's PT_DYNAMIC, as host pointers into the loaded image.
struct Dynamic {
    const Symbol *symbols = nullptr;
    const char *strings = nullptr;
    std::uint64_t strings_size = 0;
    const std::uint32_t *gnu_hash = nullptr;   // DT_GNU_HASH
    const std::uint32_t *sysv_hash = nullptr;  // DT_HASH
    std::string soname;
    std::vector<std::string> needed;
    std::span<const loader::Rela> rela;
    std::span<const loader::Rela> plt;
    std::span<const std::uint64_t> relr;
    std::span<const std::uint64_t> preinit_array;  // hold ELF addresses until relocated
    std::span<const std::uint64_t> init_array;
    std::uint64_t init = 0;  // DT_INIT, host address; 0 when there is none
    bool symbolic = false;   // DT_SYMBOLIC: the object's own definitions come first
    bool global = false;     // DF_1_GLOBAL in DT_FLAGS_1, as linked with -z global: in the global group
    // Symbol versions (DT_VERSYM, DT_VERDEF, DT_VERNEED), when the object has them.
    const std::uint16_t *versym = nullptr;             // one entry per symbol; bit 15: hidden
    std::map<std::uint16_t, std::string> defined_versions;  // index -> name, from DT_VERDEF
    std::map<std::uint16_t, std::string> needed_versions;   // index -> name, from DT_VERNEED
};

/// Reads PT_DYNAMIC of `image`. Throws LoadError when it is malformed or points outside the image.
Dynamic read_dynamic(const loader::Image &image);

/// One ELF object of a program: the program itself or one of its libraries.
struct Object {
    std::string name;  // DT_SONAME, or the name the object was asked for by
    loader::Image image;
    Dynamic dynamic;
    std::string path;  // the path the guest sees, linker64's realpath; `name` when empty
    // TLS, once the object is set up (guest_interface.h): the module ID (0 without a PT_TLS
    // segment). Objects loaded with the program have static TLS, at `tls_offset` from the thread
    // pointer; dlopen'ed ones have dynamic TLS, which exists from `tls_generation` on.
    std::uint64_t tls_module_id = 0;
    std::int64_t tls_offset = 0;
    bool static_tls = true;
    std::uint64_t tls_generation = 0;

    const std::string &shown_name() const { return path.empty() ? name : path; }
};

/// The definition of `name` that `object` exports: a defined global, weak or unique symbol with
/// default or protected visibility, found through the GNU or the SysV hash table; nullptr if none.
/// With a version, only a definition of that version counts, as linker64 checks it; without one,
/// any definition whose version is not hidden.
const Symbol *find_symbol(const Object &object, std::string_view name, std::string_view version = {});

/// The version of symbol number `index` of `object`: the version it defines, or the one it asks for
/// when the symbol is undefined; empty when there is none.
std::string_view symbol_version(const Object &object, std::uint32_t index);

/// The host address a relocation of `object` needs for its symbol number `index`: a local symbol
/// is its own; otherwise the first definition in `scope` wins (the program, then the libraries
/// breadth first), unless DT_SYMBOLIC or protected visibility keep it in the object. An undefined
/// weak symbol is 0, an undefined strong one throws LoadError.
std::uint64_t resolve(const Object &object, std::uint32_t index, std::span<const Object *const> scope);

/// The resolve log (ADR-0066 step 0): from the program's environment, the prefixes MINATO_RESOLVE_LOG lists,
/// separated by commas (for example "il2cpp_,mono_"); none without it. Called once at the program's start, before
/// any guest thread.
void watch_resolves(const char *const envp[]);

/// Whether the resolve log watches `name`: it starts with one of the prefixes.
bool watched(std::string_view name);

/// For a watched name, a line in linker64's log on stderr, "D/linker  : <how> <name> by <by>: <definer>", how the
/// name was found ("import" or "dlsym"), by which object and in which: so that a sitting learns how an engine reaches
/// its scripting runtime's API without a look into its libraries.
void note_resolved(std::string_view how, std::string_view name, std::string_view by, std::string_view definer);

/// A name the loader resolves to a definition of its own (ADR-0066): an import of `name`, or a dlsym of it, that finds
/// a definition outside `wrapper` gets `replacement`, a definition in `wrapper`, and the definition it found goes into
/// `*original`, for the replacement to call.
struct Override {
    std::string name;
    const Object *wrapper = nullptr;
    const Symbol *replacement = nullptr;
    std::uint64_t *original = nullptr;
    /// Only a definition of `name` in an object that also defines `requires_symbol` (empty for any) is replaced, and
    /// the address of that symbol goes into `*companion`, for the replacement to call: the LuaJIT switch replaces
    /// luaL_openlibs only where luaJIT_setmode is defined beside it.
    std::string requires_symbol;
    std::uint64_t *companion = nullptr;
};

/// The overrides from now on, none by default. Set at the program's start, before any guest thread.
void set_overrides(std::vector<Override> overrides);

/// More overrides, after those already set; the same time as set_overrides.
void add_overrides(std::vector<Override> overrides);

/// The override of `name`, or nullptr.
const Override *override_of(std::string_view name);

/// The override that applies to the definition of `name` found in `definer`, writing the addresses it needs into the
/// override's slots (`original`, and `companion` for a `requires_symbol`): nullptr when none applies, also when
/// `definer` lacks the required symbol or is the wrapper itself.
const Override *override_for(std::string_view name, const Object &definer, const Symbol &found);

/// Applies every relocation of `object` (RELA, PLT, RELR) except the TLS ones, resolving symbols in
/// `scope`. ifunc symbols and R_AARCH64_IRELATIVE call their resolver, which is guest code, with
/// the device's AT_HWCAP and AT_HWCAP2 (hwcap::guest, ADR-0047), as linker64 does.
void relocate(const Object &object, std::span<const Object *const> scope);

/// Host addresses of the TLS descriptor resolvers in bionic's libc.so (guest_interface.h); 0 when
/// the program has no bionic libc.
struct TlsResolvers {
    std::uint64_t static_resolver = 0;
    std::uint64_t dynamic_resolver = 0;
    std::uint64_t unresolved_weak_resolver = 0;
};

/// Applies the TLS relocations of `object` once every object's TLS module is known:
/// R_AARCH64_TLS_TPREL64, R_AARCH64_TLS_DTPMOD64, _DTPREL64, and R_AARCH64_TLSDESC with the
/// resolvers, as linker64 fills in descriptors. A variable of an object with dynamic TLS is reached
/// through TLS descriptors only; TPREL64 (initial-exec) against it throws, as on Android.
void relocate_tls(const Object &object, std::span<const Object *const> scope, const TlsResolvers &resolvers = {});

/// The number of entries of the object's symbol table, from its hash table.
std::uint32_t symbol_count(const Object &object);

/// The exported definition (global or weak) whose extent contains `address`, a host address in
/// `object`, as dladdr reports it; nullptr if none.
const Symbol *find_symbol_by_address(const Object &object, std::uint64_t address);

/// The name of a symbol of `object`.
std::string_view symbol_name(const Object &object, const Symbol &symbol);

/// The host address of `symbol`, a definition in `object` and no TLS variable: its resolver's
/// answer for an ifunc, its value for an absolute symbol.
std::uint64_t symbol_address(const Object &object, const Symbol &symbol);

/// Loads the libraries `program` needs, breadth first over DT_NEEDED, each name once, as linker64
/// does. `open` loads the library with the given DT_NEEDED name. The program comes first.
std::vector<Object> load_all(Object program, const std::function<Object(const std::string &)> &open);

/// The order in which constructors run: every object after the libraries it needs among
/// `objects`, each once (depth first, post-order from the first, the program or the library
/// dlopen asked for, which comes last).
std::vector<const Object *> initialization_order(std::span<const Object *const> objects);

/// Runs the constructors of `objects` in initialization_order: with `program`, first the program's
/// DT_PREINIT_ARRAY (linker64 ignores it in libraries), then per object DT_INIT and DT_INIT_ARRAY,
/// each with (argc, argv, envp) like linker64. Guest code: call it on a guest thread, after
/// relocate().
void run_constructors(std::span<const Object *const> objects, int argc, char **argv, char **envp, bool program);

}  // namespace apkrt::linker
