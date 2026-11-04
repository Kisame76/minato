#include "linker.h"

#include "guest_call.h"
#include "hwcap.h"

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace apkrt::linker {
namespace {

using loader::LoadError;

// ELF constants (gABI, GNU and Android extensions).
constexpr std::uint32_t pt_dynamic = 2;
constexpr std::int64_t dt_null = 0;
constexpr std::int64_t dt_needed = 1;
constexpr std::int64_t dt_pltrelsz = 2;
constexpr std::int64_t dt_hash = 4;
constexpr std::int64_t dt_strtab = 5;
constexpr std::int64_t dt_symtab = 6;
constexpr std::int64_t dt_rela = 7;
constexpr std::int64_t dt_relasz = 8;
constexpr std::int64_t dt_relaent = 9;
constexpr std::int64_t dt_strsz = 10;
constexpr std::int64_t dt_syment = 11;
constexpr std::int64_t dt_init = 12;
constexpr std::int64_t dt_soname = 14;
constexpr std::int64_t dt_symbolic = 16;
constexpr std::int64_t dt_pltrel = 20;
constexpr std::int64_t dt_jmprel = 23;
constexpr std::int64_t dt_init_array = 25;
constexpr std::int64_t dt_init_arraysz = 27;
constexpr std::int64_t dt_flags = 30;
constexpr std::int64_t dt_preinit_array = 32;
constexpr std::int64_t dt_preinit_arraysz = 33;
constexpr std::int64_t dt_relrsz = 35;
constexpr std::int64_t dt_relr = 36;
constexpr std::int64_t dt_android_relr = 0x6fff'e000;
constexpr std::int64_t dt_android_relrsz = 0x6fff'e001;
constexpr std::int64_t dt_gnu_hash = 0x6fff'fef5;
constexpr std::int64_t dt_versym = 0x6fff'fff0;
constexpr std::int64_t dt_verdef = 0x6fff'fffc;
constexpr std::int64_t dt_verdefnum = 0x6fff'fffd;
constexpr std::int64_t dt_verneed = 0x6fff'fffe;
constexpr std::int64_t dt_verneednum = 0x6fff'ffff;
constexpr std::int64_t dt_flags_1 = 0x6fff'fffb;
constexpr std::uint64_t df_symbolic = 0x2;
constexpr std::uint64_t df_1_global = 0x2;

constexpr std::uint16_t versym_global = 1;
constexpr std::uint16_t versym_hidden = 0x8000;

constexpr std::uint32_t r_aarch64_tls_dtpmod64 = 1028;
constexpr std::uint32_t r_aarch64_tls_dtprel64 = 1029;
constexpr std::uint32_t r_aarch64_tls_tprel64 = 1030;
constexpr std::uint32_t r_aarch64_tlsdesc = 1031;
constexpr std::uint32_t r_aarch64_irelative = 1032;

// Elf64_Verdef, Elf64_Verdaux, Elf64_Verneed, Elf64_Vernaux.
struct Verdef {
    std::uint16_t version, flags, index, count;
    std::uint32_t hash, aux, next;
};
struct Verdaux {
    std::uint32_t name, next;
};
struct Verneed {
    std::uint16_t version, count;
    std::uint32_t file, aux, next;
};
struct Vernaux {
    std::uint32_t hash;
    std::uint16_t flags, other;
    std::uint32_t name, next;
};

constexpr std::uint16_t shn_undef = 0;
constexpr std::uint16_t shn_abs = 0xfff1;
constexpr unsigned stb_local = 0;
constexpr unsigned stb_global = 1;
constexpr unsigned stb_weak = 2;
constexpr unsigned stb_gnu_unique = 10;
constexpr unsigned stt_tls = 6;
constexpr unsigned stt_gnu_ifunc = 10;
constexpr unsigned stv_default = 0;
constexpr unsigned stv_protected = 3;

struct ProgramHeader {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t vaddr;
    std::uint64_t paddr;
    std::uint64_t file_size;
    std::uint64_t memory_size;
    std::uint64_t alignment;
};

struct DynamicEntry {
    std::int64_t tag;
    std::uint64_t value;
};

unsigned binding(const Symbol &symbol) { return symbol.info >> 4; }
unsigned type(const Symbol &symbol) { return symbol.info & 0xf; }
unsigned visibility(const Symbol &symbol) { return symbol.other & 3; }

/// The host address range of a loaded image; every table the linker reads must lie inside it.
class Bounds {
public:
    explicit Bounds(const loader::Image &image)
        : begin_(image.bias + image.info->min_vaddr), end_(begin_ + image.info->image_size), bias_(image.bias) {}

    /// Host pointer to `count` objects of type T at ELF address `address`.
    template <typename T>
    const T *at(std::uint64_t address, std::uint64_t count, const char *what) const {
        const auto host = bias_ + address;
        if (host < begin_ || host > end_ || count > (end_ - host) / sizeof(T)) {
            throw LoadError(std::format("the {} at {:#x} lies outside the image", what, address));
        }
        return reinterpret_cast<const T *>(host);
    }

    std::uint64_t end() const { return end_; }

private:
    std::uint64_t begin_;
    std::uint64_t end_;
    std::uint64_t bias_;
};

/// The string at `offset` in the string table, which must end inside the table.
std::string_view string_at(const Dynamic &dynamic, std::uint64_t offset) {
    if (dynamic.strings == nullptr || offset >= dynamic.strings_size) {
        throw LoadError(std::format("string {:#x} lies outside the string table", offset));
    }
    const auto *start = dynamic.strings + offset;
    const auto length = ::strnlen(start, dynamic.strings_size - offset);
    if (length == dynamic.strings_size - offset) throw LoadError("the string table does not end with a zero");
    return {start, length};
}

bool is_exported(const Symbol &symbol) {
    const auto bind = binding(symbol);
    const auto visible = visibility(symbol);
    return symbol.section != shn_undef && (bind == stb_global || bind == stb_weak || bind == stb_gnu_unique) &&
           (visible == stv_default || visible == stv_protected);
}

/// The version entry of symbol `index`, checked against the image; versym_global without a table.
std::uint16_t version_entry(const Object &object, std::uint32_t index) {
    const auto *versym = object.dynamic.versym;
    if (versym == nullptr) return versym_global;
    if (reinterpret_cast<std::uint64_t>(versym + index) + 2 > Bounds(object.image).end()) {
        throw LoadError(std::format("{}: the version table ends before symbol {}", object.name, index));
    }
    return versym[index];
}

/// linker64's check_symbol_version: without a version asked for, a definition counts unless its
/// version is hidden; with one, only a definition of that version, or an unversioned one when the
/// object does not define the version at all.
bool version_matches(const Object &definer, std::uint32_t index, std::string_view version) {
    if (definer.dynamic.versym == nullptr) return true;
    const auto entry = version_entry(definer, index);
    if (version.empty()) return (entry & versym_hidden) == 0;
    std::uint16_t wanted = versym_global;
    for (const auto &[number, name] : definer.dynamic.defined_versions) {
        if (name == version) wanted = number;
    }
    return wanted == (entry & ~versym_hidden);
}

/// Whether the symbol at `index` is the exported definition of `name` in `version`. The index comes
/// from a hash table of the file, so it is checked against the image before the symbol is read.
const Symbol *match(const Object &object, std::uint32_t index, std::string_view name, std::string_view version) {
    const auto &dynamic = object.dynamic;
    const auto address = reinterpret_cast<std::uint64_t>(dynamic.symbols + index);
    if (address + sizeof(Symbol) > Bounds(object.image).end()) {
        throw LoadError(std::format("{}: the hash table names symbol {} beyond the image", object.name, index));
    }
    const auto &symbol = dynamic.symbols[index];
    if (!is_exported(symbol) || symbol.name >= dynamic.strings_size) return nullptr;
    if (string_at(dynamic, symbol.name) != name || !version_matches(object, index, version)) return nullptr;
    return &symbol;
}

/// GNU hash (DT_GNU_HASH): a Bloom filter, then one bucket and its chain, whose entries end with
/// the low bit set. Only symbols from symoffset on are hashed.
const Symbol *find_gnu(const Object &object, std::string_view name, std::string_view version) {
    const auto *table = object.dynamic.gnu_hash;
    const std::uint32_t buckets = table[0], symbol_offset = table[1], bloom_size = table[2], bloom_shift = table[3];
    if (buckets == 0 || bloom_size == 0) return nullptr;
    std::uint32_t hash = 5381;
    for (const char c : name) hash = hash * 33 + static_cast<unsigned char>(c);

    const auto *bloom = reinterpret_cast<const std::uint64_t *>(table + 4);
    const auto word = bloom[(hash / 64) % bloom_size];
    const auto mask = (std::uint64_t{1} << (hash % 64)) | (std::uint64_t{1} << ((hash >> bloom_shift) % 64));
    if ((word & mask) != mask) return nullptr;

    const auto *bucket = reinterpret_cast<const std::uint32_t *>(bloom + bloom_size);
    const auto *chain = bucket + buckets;
    const auto end = Bounds(object.image).end();
    for (std::uint32_t index = bucket[hash % buckets]; index >= symbol_offset; ++index) {
        const auto *entry = chain + (index - symbol_offset);
        if (reinterpret_cast<std::uint64_t>(entry) + 4 > end) {
            throw LoadError(std::format("{}: the GNU hash chain runs beyond the image", object.name));
        }
        if ((*entry | 1) == (hash | 1)) {
            if (const auto *symbol = match(object, index, name, version)) return symbol;
        }
        if ((*entry & 1) != 0) break;
    }
    return nullptr;
}

/// SysV hash (DT_HASH): buckets and chains of symbol numbers, ended by 0.
const Symbol *find_sysv(const Object &object, std::string_view name, std::string_view version) {
    const auto *table = object.dynamic.sysv_hash;
    const std::uint32_t buckets = table[0], chains = table[1];
    if (buckets == 0) return nullptr;
    std::uint32_t hash = 0;
    for (const char c : name) {
        hash = (hash << 4) + static_cast<unsigned char>(c);
        const auto high = hash & 0xf000'0000;
        if (high != 0) hash ^= high >> 24;
        hash &= ~high;
    }
    const auto *bucket = table + 2;
    const auto *chain = bucket + buckets;
    std::uint32_t steps = 0;
    for (auto index = bucket[hash % buckets]; index != 0; index = chain[index]) {
        if (index >= chains || ++steps > chains) {
            throw LoadError(std::format("{}: the SysV hash chain is broken", object.name));
        }
        if (const auto *symbol = match(object, index, name, version)) return symbol;
    }
    return nullptr;
}

/// Calls an ifunc resolver, guest code, the way bionic's __bionic_call_ifunc_resolver does on arm64.
std::uint64_t call_ifunc_resolver(std::uint64_t resolver) {
    struct Argument {  // __ifunc_arg_t
        std::uint64_t size;
        std::uint64_t hwcap;
        std::uint64_t hwcap2;
    };
    static const Argument argument{sizeof(Argument), hwcap::guest().hwcap, hwcap::guest().hwcap2};
    constexpr std::uint64_t ifunc_arg_hwcap = std::uint64_t{1} << 62;  // _IFUNC_ARG_HWCAP
    using Resolver = std::uint64_t (*)(std::uint64_t, const Argument *);
    return call_guest(reinterpret_cast<Resolver>(resolver), argument.hwcap | ifunc_arg_hwcap, &argument);
}

/// The host address of `symbol`, which `object` defines.
std::uint64_t address_of(const Object &object, const Symbol &symbol, std::string_view name) {
    if (type(symbol) == stt_gnu_ifunc) return call_ifunc_resolver(object.image.bias + symbol.value);
    if (type(symbol) == stt_tls) {
        throw LoadError(std::format("\"{}\" in \"{}\" is a TLS symbol, not supported yet (M2 step 5b)", name, object.name));
    }
    return symbol.section == shn_abs ? symbol.value : object.image.bias + symbol.value;
}

}  // namespace

Dynamic read_dynamic(const loader::Image &image) {
    const auto &info = *image.info;
    const Bounds bounds(image);
    Dynamic dynamic;
    if (info.phdr == 0) return dynamic;
    const auto *headers = bounds.at<ProgramHeader>(info.phdr, info.phnum, "program header table");
    const ProgramHeader *segment = nullptr;
    for (std::uint32_t index = 0; index < info.phnum; ++index) {
        if (headers[index].type == pt_dynamic) segment = &headers[index];
    }
    if (segment == nullptr) return dynamic;

    std::vector<std::uint64_t> needed;
    std::uint64_t soname = ~std::uint64_t{0}, strtab = 0, symtab = 0, hash = 0, gnu_hash = 0;
    std::uint64_t rela = 0, rela_size = 0, jmprel = 0, jmprel_size = 0, relr = 0, relr_size = 0;
    std::uint64_t init_array = 0, init_array_size = 0, preinit_array = 0, preinit_array_size = 0;
    std::uint64_t versym = 0, verdef = 0, verdef_count = 0, verneed = 0, verneed_count = 0;
    const auto count = segment->memory_size / sizeof(DynamicEntry);
    const auto *entries = bounds.at<DynamicEntry>(segment->vaddr, count, "dynamic section");
    for (std::uint64_t index = 0; index < count && entries[index].tag != dt_null; ++index) {
        const auto value = entries[index].value;
        switch (entries[index].tag) {
            case dt_needed: needed.push_back(value); break;
            case dt_soname: soname = value; break;
            case dt_strtab: strtab = value; break;
            case dt_strsz: dynamic.strings_size = value; break;
            case dt_symtab: symtab = value; break;
            case dt_syment:
                if (value != sizeof(Symbol)) throw LoadError(std::format("symbol entries of {} bytes", value));
                break;
            case dt_hash: hash = value; break;
            case dt_gnu_hash: gnu_hash = value; break;
            case dt_rela: rela = value; break;
            case dt_relasz: rela_size = value; break;
            case dt_relaent:
                if (value != sizeof(loader::Rela)) throw LoadError(std::format("RELA entries of {} bytes", value));
                break;
            case dt_jmprel: jmprel = value; break;
            case dt_pltrelsz: jmprel_size = value; break;
            case dt_pltrel:
                if (value != static_cast<std::uint64_t>(dt_rela)) throw LoadError("PLT relocations are no RELA");
                break;
            case dt_relr:
            case dt_android_relr: relr = value; break;
            case dt_relrsz:
            case dt_android_relrsz: relr_size = value; break;
            case dt_init: dynamic.init = image.bias + value; break;
            case dt_init_array: init_array = value; break;
            case dt_init_arraysz: init_array_size = value; break;
            case dt_preinit_array: preinit_array = value; break;
            case dt_preinit_arraysz: preinit_array_size = value; break;
            case dt_symbolic: dynamic.symbolic = true; break;
            case dt_versym: versym = value; break;
            case dt_verdef: verdef = value; break;
            case dt_verdefnum: verdef_count = value; break;
            case dt_verneed: verneed = value; break;
            case dt_verneednum: verneed_count = value; break;
            case dt_flags:
                if ((value & df_symbolic) != 0) dynamic.symbolic = true;
                break;
            case dt_flags_1:
                if ((value & df_1_global) != 0) dynamic.global = true;
                break;
            default: break;
        }
    }

    if (strtab != 0) dynamic.strings = bounds.at<char>(strtab, dynamic.strings_size, "string table");
    if (symtab != 0) dynamic.symbols = bounds.at<Symbol>(symtab, 1, "symbol table");
    if (gnu_hash != 0) {
        const auto *header = bounds.at<std::uint32_t>(gnu_hash, 4, "GNU hash table");
        const std::uint64_t words = 4 + 2 * std::uint64_t{header[2]} + header[0];  // header, Bloom filter, buckets
        dynamic.gnu_hash = bounds.at<std::uint32_t>(gnu_hash, words, "GNU hash table");
    }
    if (hash != 0) {
        const auto *header = bounds.at<std::uint32_t>(hash, 2, "SysV hash table");
        dynamic.sysv_hash = bounds.at<std::uint32_t>(hash, 2 + std::uint64_t{header[0]} + header[1], "SysV hash table");
    }
    const auto rela_span = [&](std::uint64_t address, std::uint64_t size, const char *what) {
        const auto rows = size / sizeof(loader::Rela);
        return address == 0 ? std::span<const loader::Rela>() : std::span(bounds.at<loader::Rela>(address, rows, what), rows);
    };
    const auto word_span = [&](std::uint64_t address, std::uint64_t size, const char *what) {
        const auto words = size / 8;
        return address == 0 ? std::span<const std::uint64_t>() : std::span(bounds.at<std::uint64_t>(address, words, what), words);
    };
    dynamic.rela = rela_span(rela, rela_size, "RELA table");
    dynamic.plt = rela_span(jmprel, jmprel_size, "PLT relocations");
    dynamic.relr = word_span(relr, relr_size, "RELR table");
    dynamic.init_array = word_span(init_array, init_array_size, "init array");
    dynamic.preinit_array = word_span(preinit_array, preinit_array_size, "preinit array");

    for (const auto offset : needed) dynamic.needed.emplace_back(string_at(dynamic, offset));
    if (soname != ~std::uint64_t{0}) dynamic.soname = string_at(dynamic, soname);

    // Versions: the names by index, from the definitions and from the requirements.
    if (versym != 0) dynamic.versym = bounds.at<std::uint16_t>(versym, 1, "version table");
    std::uint64_t at = verdef;
    for (std::uint64_t index = 0; verdef != 0 && index < verdef_count; ++index) {
        const auto *definition = bounds.at<Verdef>(at, 1, "version definition");
        if (definition->count > 0) {
            const auto *aux = bounds.at<Verdaux>(at + definition->aux, 1, "version definition");
            dynamic.defined_versions[definition->index] = std::string(string_at(dynamic, aux->name));
        }
        if (definition->next == 0) break;
        at += definition->next;
    }
    at = verneed;
    for (std::uint64_t index = 0; verneed != 0 && index < verneed_count; ++index) {
        const auto *requirement = bounds.at<Verneed>(at, 1, "version requirement");
        std::uint64_t aux_at = at + requirement->aux;
        for (std::uint16_t entry = 0; entry < requirement->count; ++entry) {
            const auto *aux = bounds.at<Vernaux>(aux_at, 1, "version requirement");
            dynamic.needed_versions[aux->other] = std::string(string_at(dynamic, aux->name));
            if (aux->next == 0) break;
            aux_at += aux->next;
        }
        if (requirement->next == 0) break;
        at += requirement->next;
    }
    return dynamic;
}

const Symbol *find_symbol(const Object &object, std::string_view name, std::string_view version) {
    if (object.dynamic.symbols == nullptr || object.dynamic.strings == nullptr) return nullptr;
    if (object.dynamic.gnu_hash != nullptr) return find_gnu(object, name, version);
    if (object.dynamic.sysv_hash != nullptr) return find_sysv(object, name, version);
    return nullptr;
}

std::uint32_t symbol_count(const Object &object) {
    const auto &dynamic = object.dynamic;
    if (dynamic.sysv_hash != nullptr) return dynamic.sysv_hash[1];  // nchain
    if (dynamic.gnu_hash == nullptr) return 0;
    // GNU hash: past the highest bucket's chain, whose last entry has the low bit set.
    const auto *table = dynamic.gnu_hash;
    const std::uint32_t buckets = table[0], symbol_offset = table[1], bloom_size = table[2];
    const auto *bucket = reinterpret_cast<const std::uint32_t *>(reinterpret_cast<const std::uint64_t *>(table + 4) +
                                                                 bloom_size);
    const auto *chain = bucket + buckets;
    std::uint32_t highest = 0;
    for (std::uint32_t index = 0; index < buckets; ++index) highest = std::max(highest, bucket[index]);
    if (highest < symbol_offset) return symbol_offset;
    const auto end = Bounds(object.image).end();
    while (true) {
        const auto *entry = chain + (highest - symbol_offset);
        if (reinterpret_cast<std::uint64_t>(entry) + 4 > end) {
            throw LoadError(std::format("{}: the GNU hash chain runs beyond the image", object.name));
        }
        if ((*entry & 1) != 0) return highest + 1;
        ++highest;
    }
}

const Symbol *find_symbol_by_address(const Object &object, std::uint64_t address) {
    const auto &dynamic = object.dynamic;
    if (dynamic.symbols == nullptr || address < object.image.bias) return nullptr;
    const auto offset = address - object.image.bias;
    const auto count = symbol_count(object);
    const auto end = Bounds(object.image).end();
    for (std::uint32_t index = 1; index < count; ++index) {
        const auto *symbol = dynamic.symbols + index;
        if (reinterpret_cast<std::uint64_t>(symbol) + sizeof(Symbol) > end) break;
        const auto bind = binding(*symbol);
        if (symbol->section == shn_undef || (bind != stb_global && bind != stb_weak)) continue;
        if (offset >= symbol->value && offset < symbol->value + symbol->size) return symbol;
    }
    return nullptr;
}

std::string_view symbol_name(const Object &object, const Symbol &symbol) {
    return string_at(object.dynamic, symbol.name);
}

std::uint64_t symbol_address(const Object &object, const Symbol &symbol) {
    return address_of(object, symbol, symbol_name(object, symbol));
}

std::string_view symbol_version(const Object &object, std::uint32_t index) {
    const auto entry = static_cast<std::uint16_t>(version_entry(object, index) & ~versym_hidden);
    if (entry <= versym_global) return {};
    const auto &dynamic = object.dynamic;
    const auto &table = dynamic.symbols[index].section == shn_undef ? dynamic.needed_versions : dynamic.defined_versions;
    const auto found = table.find(entry);
    if (found == table.end()) throw LoadError(std::format("{}: symbol {} has an unknown version {}", object.name, index, entry));
    return found->second;
}

namespace {

struct Definition {
    const Object *object = nullptr;  // null for an undefined weak symbol
    const Symbol *symbol = nullptr;
    std::string_view name;
};

/// Where symbol number `index` of `object` is defined, by the rules of resolve().
Definition lookup(const Object &object, std::uint32_t index, std::span<const Object *const> scope) {
    const auto &dynamic = object.dynamic;
    if (dynamic.symbols == nullptr) throw LoadError(std::format("{}: relocation with a symbol, but no symbol table", object.name));
    const auto *symbol = dynamic.symbols + index;
    if (reinterpret_cast<std::uint64_t>(symbol) + sizeof(Symbol) > Bounds(object.image).end()) {
        throw LoadError(std::format("{}: relocation with symbol {} beyond the image", object.name, index));
    }
    const auto name = string_at(dynamic, symbol->name);

    const bool defined = symbol->section != shn_undef;
    if (binding(*symbol) == stb_local) {
        if (!defined) throw LoadError(std::format("{}: undefined local symbol \"{}\"", object.name, name));
        return {&object, symbol, name};
    }
    if (defined && (dynamic.symbolic || visibility(*symbol) == stv_protected)) return {&object, symbol, name};
    const auto version = symbol_version(object, index);
    for (const auto *candidate : scope) {
        if (const auto *definition = find_symbol(*candidate, name, version)) {
            note_resolved("import", name, object.name, candidate->name);
            // Never for the wrapper's own lookups, which reach the definition it stands for.
            if (const auto *taken = override_for(name, *candidate, *definition);
                taken != nullptr && &object != taken->wrapper) {
                return {taken->wrapper, taken->replacement, name};
            }
            return {candidate, definition, name};
        }
    }
    if (binding(*symbol) == stb_weak) return {nullptr, nullptr, name};
    // linker64's words, the version left out as there.
    throw LoadError(std::format("cannot locate symbol \"{}\" referenced by \"{}\"...", name, object.shown_name()));
}

/// MINATO_RESOLVE_LOG's prefixes, set before any guest thread and only read after.
std::vector<std::string> watched_prefixes;

/// The overrides (ADR-0066), likewise.
std::vector<Override> overrides;

bool is_tls_relocation(std::uint32_t type) {
    return type == r_aarch64_tls_dtpmod64 || type == r_aarch64_tls_dtprel64 || type == r_aarch64_tls_tprel64 ||
           type == r_aarch64_tlsdesc;
}

}  // namespace

void set_overrides(std::vector<Override> taken) {
    overrides = std::move(taken);
}

void add_overrides(std::vector<Override> taken) {
    for (auto &one : taken) overrides.push_back(std::move(one));
}

const Override *override_for(std::string_view name, const Object &definer, const Symbol &found) {
    const auto *taken = override_of(name);
    if (taken == nullptr || &definer == taken->wrapper) return nullptr;
    const Symbol *companion = nullptr;
    if (!taken->requires_symbol.empty()) {
        companion = find_symbol(definer, taken->requires_symbol);
        if (companion == nullptr) return nullptr;
    }
    *taken->original = symbol_address(definer, found);
    if (companion != nullptr && taken->companion != nullptr) *taken->companion = symbol_address(definer, *companion);
    return taken;
}

const Override *override_of(std::string_view name) {
    for (const auto &taken : overrides) {
        if (taken.name == name) return &taken;
    }
    return nullptr;
}

void watch_resolves(const char *const envp[]) {
    watched_prefixes.clear();
    constexpr std::string_view variable = "MINATO_RESOLVE_LOG=";
    for (; envp != nullptr && *envp != nullptr; ++envp) {
        std::string_view value(*envp);
        if (!value.starts_with(variable)) continue;
        value.remove_prefix(variable.size());
        while (!value.empty()) {
            const auto comma = value.find(',');
            const auto part = value.substr(0, comma);
            value = comma == std::string_view::npos ? std::string_view{} : value.substr(comma + 1);
            if (!part.empty()) watched_prefixes.emplace_back(part);
        }
    }
}

bool watched(std::string_view name) {
    return std::ranges::any_of(watched_prefixes, [&](const std::string &prefix) { return name.starts_with(prefix); });
}

void note_resolved(std::string_view how, std::string_view name, std::string_view by, std::string_view definer) {
    if (!watched(name)) return;
    const auto line = std::format("D/{:<8}: {} {} by {}: {}\n", "linker", how, name, by, definer);
    static_cast<void>(::write(STDERR_FILENO, line.data(), line.size()));
}

std::uint64_t resolve(const Object &object, std::uint32_t index, std::span<const Object *const> scope) {
    const auto definition = lookup(object, index, scope);
    return definition.object == nullptr ? 0 : address_of(*definition.object, *definition.symbol, definition.name);
}

void relocate(const Object &object, std::span<const Object *const> scope) {
    const auto writable = loader::writable_part(object.image);
    const auto resolver = [&](std::uint32_t index) { return resolve(object, index, scope); };
    // R_AARCH64_IRELATIVE here; the TLS types wait for relocate_tls.
    const auto other = [&](const loader::Rela &entry, std::uint64_t *target) {
        const auto type = static_cast<std::uint32_t>(entry.info);
        if (type == r_aarch64_irelative) {
            *target = call_ifunc_resolver(object.image.bias + static_cast<std::uint64_t>(entry.addend));
            return true;
        }
        return is_tls_relocation(type);
    };
    loader::apply_relr(object.dynamic.relr, object.image.bias, writable);
    loader::apply_rela(object.dynamic.rela, object.image.bias, writable, resolver, other);
    loader::apply_rela(object.dynamic.plt, object.image.bias, writable, resolver, other);
}

void relocate_tls(const Object &object, std::span<const Object *const> scope, const TlsResolvers &resolvers) {
    const auto writable = loader::writable_part(object.image);
    for (const auto table : {object.dynamic.rela, object.dynamic.plt}) {
        for (const auto &entry : table) {
            const auto type = static_cast<std::uint32_t>(entry.info);
            if (!is_tls_relocation(type)) continue;
            const std::uint64_t size = type == r_aarch64_tlsdesc ? 16 : 8;
            if (!writable.contains(entry.offset, size)) {
                throw LoadError(std::format("{}: TLS relocation at {:#x} lies outside the writable part", object.name,
                                            entry.offset));
            }
            auto *const target = reinterpret_cast<std::uint64_t *>(object.image.bias + entry.offset);
            const auto addend = static_cast<std::uint64_t>(entry.addend);
            // The variable's object and offset in its TLS block (symbol 0: the object itself).
            const auto index = static_cast<std::uint32_t>(entry.info >> 32);
            Definition definition{&object, nullptr, {}};
            if (index != 0) definition = lookup(object, index, scope);
            const auto value = definition.symbol == nullptr ? 0 : definition.symbol->value;

            if (type == r_aarch64_tlsdesc) {
                if (resolvers.static_resolver == 0) {
                    throw LoadError(std::format("{}: TLS descriptors need bionic's libc.so", object.name));
                }
                if (definition.object == nullptr) {
                    // An undefined weak variable: its address comes out as the addend, NULL for 0.
                    target[0] = resolvers.unresolved_weak_resolver;
                    target[1] = addend;
                    continue;
                }
            }
            if (definition.object == nullptr) {
                target[0] = 0;  // DTPMOD64, DTPREL64, TPREL64 of an undefined weak variable
                continue;
            }
            if (definition.object->tls_module_id == 0) {
                throw LoadError(std::format("{}: TLS relocation at {:#x} against \"{}\", which has no TLS", object.name,
                                            entry.offset, definition.object->name));
            }
            const auto &definer = *definition.object;
            switch (type) {
                case r_aarch64_tls_dtpmod64: *target = definer.tls_module_id; break;
                case r_aarch64_tls_dtprel64: *target = value + addend; break;
                case r_aarch64_tls_tprel64:
                    if (!definer.static_tls) {
                        throw LoadError(std::format(
                            "TLS symbol \"{}\" in dlopened \"{}\" referenced from \"{}\" using IE access model",
                            definition.name, definer.shown_name(), object.shown_name()));
                    }
                    // The offset of the variable from the thread pointer, in the static TLS block.
                    *target = static_cast<std::uint64_t>(definer.tls_offset) + value + addend;
                    break;
                case r_aarch64_tlsdesc:
                    if (definer.static_tls) {
                        target[0] = resolvers.static_resolver;
                        target[1] = static_cast<std::uint64_t>(definer.tls_offset) + value + addend;
                    } else {
                        // TlsDynamicResolverArg {generation, TlsIndex {module_id, offset}}; linker64
                        // keeps these as long as the library, which here is forever.
                        auto *argument = new std::uint64_t[3]{definer.tls_generation, definer.tls_module_id,
                                                              value + addend};
                        target[0] = resolvers.dynamic_resolver;
                        target[1] = reinterpret_cast<std::uint64_t>(argument);
                    }
                    break;
            }
        }
    }
}

std::vector<Object> load_all(Object program, const std::function<Object(const std::string &)> &open) {
    std::vector<Object> objects;
    std::set<std::string> known{program.name};
    objects.push_back(std::move(program));
    for (std::size_t next = 0; next < objects.size(); ++next) {
        const auto needed = objects[next].dynamic.needed;  // a copy: push_back moves the objects
        for (const auto &name : needed) {
            if (!known.insert(name).second) continue;
            objects.push_back(open(name));
        }
    }
    return objects;
}

std::vector<const Object *> initialization_order(std::span<const Object *const> objects) {
    std::unordered_map<std::string, const Object *> by_name;
    for (const auto *object : objects) by_name.emplace(object->name, object);
    std::vector<const Object *> order;
    std::set<const Object *> visited;
    // Marked before its children, as linker64 does, so that a cycle ends.
    const std::function<void(const Object &)> visit = [&](const Object &object) {
        if (!visited.insert(&object).second) return;
        for (const auto &name : object.dynamic.needed) {
            if (const auto found = by_name.find(name); found != by_name.end()) visit(*found->second);
        }
        order.push_back(&object);
    };
    if (!objects.empty()) visit(*objects.front());
    return order;
}

void run_constructors(std::span<const Object *const> objects, int argc, char **argv, char **envp, bool program) {
    using Constructor = void (*)(int, char **, char **);
    // Entries of 0 and -1 are skipped, as linker64 does.
    const auto call = [&](std::uint64_t address) {
        if (address != 0 && address != ~std::uint64_t{0}) call_guest(reinterpret_cast<Constructor>(address), argc, argv, envp);
    };
    if (objects.empty()) return;
    if (program) {
        for (const auto address : objects.front()->dynamic.preinit_array) call(address);
    }
    for (const auto *object : initialization_order(objects)) {
        call(object->dynamic.init);
        for (const auto address : object->dynamic.init_array) call(address);
    }
}

}  // namespace apkrt::linker
