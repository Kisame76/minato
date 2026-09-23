// Checks the linker (ADR-0021) on the NDK fixtures raw-dyn, libraw-dyn.so and libraw-dyn-sysv.so,
// laid out in memory the way their LOAD segments ask: the dynamic section, symbol lookup through
// the GNU and the SysV hash table against a plain scan of the symbol table, which definition a
// relocation gets, the load order and the order of the constructors.
// Symbol versions are checked on our own build of bionic's libc.so, when it is there.
//   linker_test <fixtures-directory> [<bionic-library-directory>]
//   (exit status 77, skipped, while the fixtures are missing)
#include "linker.h"

#include "check.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace {

using apkrt::linker::Object;

std::filesystem::path fixtures;
std::filesystem::path bionic;  // empty, or where tools/build-bionic.sh put libc.so

constexpr std::uint32_t pt_load = 1;
constexpr std::uint32_t pt_phdr = 6;
constexpr std::uint32_t sht_dynsym = 11;
constexpr std::uint64_t page = 0x4000;

template <typename T>
T read_at(const std::vector<std::byte> &bytes, std::size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

/// An ELF file laid out in memory by its LOAD segments, the way dyld lays out a container.
struct Mapped {
    std::vector<std::byte> file;
    std::vector<std::byte> memory;
    apk_container_info info{};
    Object object;
    std::uint64_t symbol_count = 0;  // from the section headers, for the plain scan

    std::uint64_t bias() const { return object.image.bias; }
};

std::unique_ptr<Mapped> map_elf(const std::filesystem::path &path, const std::string &name) {
    auto mapped = std::make_unique<Mapped>();
    std::ifstream stream(path.is_absolute() ? path : fixtures / path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    mapped->file.resize(raw.size());
    std::memcpy(mapped->file.data(), raw.data(), raw.size());
    const auto &file = mapped->file;

    const auto phoff = read_at<std::uint64_t>(file, 0x20);
    const auto phnum = read_at<std::uint16_t>(file, 0x38);
    std::uint64_t end = 0;
    for (std::uint16_t index = 0; index < phnum; ++index) {
        const auto at = phoff + 56u * index;
        const auto type = read_at<std::uint32_t>(file, at);
        const auto flags = read_at<std::uint32_t>(file, at + 4);
        const auto vaddr = read_at<std::uint64_t>(file, at + 16);
        const auto memory_size = read_at<std::uint64_t>(file, at + 40);
        if (type == pt_phdr) mapped->info.phdr = vaddr;
        if (type != pt_load) continue;
        end = std::max(end, (vaddr + memory_size + page - 1) & ~(page - 1));
        if ((flags & 2) != 0 && mapped->info.writable_start == 0) mapped->info.writable_start = vaddr & ~(page - 1);
    }
    mapped->memory.resize(end);
    for (std::uint16_t index = 0; index < phnum; ++index) {
        const auto at = phoff + 56u * index;
        if (read_at<std::uint32_t>(file, at) != pt_load) continue;
        const auto offset = read_at<std::uint64_t>(file, at + 8);
        const auto vaddr = read_at<std::uint64_t>(file, at + 16);
        const auto file_size = read_at<std::uint64_t>(file, at + 32);
        std::memcpy(mapped->memory.data() + vaddr, file.data() + offset, file_size);
        if (mapped->info.phdr == 0 && phoff >= offset && phoff < offset + file_size) {
            mapped->info.phdr = vaddr + (phoff - offset);
        }
    }
    mapped->info.phnum = phnum;
    mapped->info.phentsize = 56;
    mapped->info.min_vaddr = 0;
    mapped->info.image_size = end;
    mapped->info.writable_end = end;

    const auto shoff = read_at<std::uint64_t>(file, 0x28);
    const auto shnum = read_at<std::uint16_t>(file, 0x3c);
    for (std::uint16_t index = 0; index < shnum; ++index) {
        const auto at = shoff + 64u * index;
        if (read_at<std::uint32_t>(file, at + 4) == sht_dynsym) {
            mapped->symbol_count = read_at<std::uint64_t>(file, at + 32) / sizeof(apkrt::linker::Symbol);
        }
    }

    const apkrt::loader::Image image{
        .header = nullptr,
        .info = &mapped->info,
        .bias = reinterpret_cast<std::uint64_t>(mapped->memory.data()),
    };
    mapped->object = Object{.name = name, .image = image, .dynamic = apkrt::linker::read_dynamic(image)};
    return mapped;
}

/// The symbol number of `name` in the object's symbol table, found by a plain scan.
std::uint32_t symbol_index(const Mapped &mapped, std::string_view name) {
    const auto &dynamic = mapped.object.dynamic;
    for (std::uint32_t index = 1; index < mapped.symbol_count; ++index) {
        if (name == dynamic.strings + dynamic.symbols[index].name) return index;
    }
    throw std::runtime_error("no symbol " + std::string(name));
}

bool is_exported_definition(const apkrt::linker::Symbol &symbol) {
    const unsigned binding = symbol.info >> 4, visibility = symbol.other & 3;
    return symbol.section != 0 && (binding == 1 || binding == 2 || binding == 10) && (visibility == 0 || visibility == 3);
}

/// Every symbol of the table must be found through the hash table exactly when it is an exported
/// definition; names that are not in the table must not be found.
void check_lookup_matches_a_plain_scan(const Mapped &mapped) {
    const auto &dynamic = mapped.object.dynamic;
    CHECK(mapped.symbol_count > 1);
    for (std::uint32_t index = 1; index < mapped.symbol_count; ++index) {
        const auto &symbol = dynamic.symbols[index];
        const auto *found = apkrt::linker::find_symbol(mapped.object, dynamic.strings + symbol.name);
        CHECK(found == (is_exported_definition(symbol) ? &symbol : nullptr));
    }
    for (const char *absent : {"", "raw_dyn_missing", "raw_dyn_prin", "raw_dyn_print2", "raw_dyn_printx"}) {
        CHECK(apkrt::linker::find_symbol(mapped.object, absent) == nullptr);
    }
}

TEST(the_dynamic_section_names_libraries_tables_and_constructors) {
    const auto program = map_elf("raw-dyn", "raw-dyn");
    const auto library = map_elf("libraw-dyn.so", "libraw-dyn.so");
    CHECK(program->object.dynamic.needed == std::vector<std::string>{"libraw-dyn.so"});
    CHECK(program->object.dynamic.soname.empty());
    CHECK(library->object.dynamic.soname == "libraw-dyn.so");
    CHECK(library->object.dynamic.needed.empty());
    CHECK(library->object.dynamic.init_array.size() == 1);
    CHECK(library->object.dynamic.plt.size() == 1);  // raw_dyn_greeting
    CHECK(library->object.dynamic.gnu_hash != nullptr);
    CHECK(library->object.dynamic.symbols != nullptr && library->object.dynamic.strings != nullptr);
}

TEST(symbols_are_found_through_the_gnu_hash_table) {
    check_lookup_matches_a_plain_scan(*map_elf("libraw-dyn.so", "libraw-dyn.so"));
    check_lookup_matches_a_plain_scan(*map_elf("raw-dyn", "raw-dyn"));  // raw_dyn_print is undefined there
}

/// GNU hash of `name`, as the linker computes it.
std::uint32_t gnu_hash(std::string_view name) {
    std::uint32_t hash = 5381;
    for (const char c : name) hash = hash * 33 + static_cast<unsigned char>(c);
    return hash;
}

TEST(a_name_that_passes_the_bloom_filter_ends_at_its_chain) {
    // Look for a name the table does not hold but whose two Bloom bits are set: its lookup must
    // walk a chain and stop at the chain's end.
    const auto library = map_elf("libraw-dyn.so", "libraw-dyn.so");
    const auto *table = library->object.dynamic.gnu_hash;
    const std::uint32_t buckets = table[0], bloom_size = table[2], bloom_shift = table[3];
    const auto *bloom = reinterpret_cast<const std::uint64_t *>(table + 4);
    const auto *bucket = reinterpret_cast<const std::uint32_t *>(bloom + bloom_size);
    int tried = 0;
    std::uint64_t state = 0x9e37'79b9'7f4a'7c15ull;
    for (int number = 0; number < 100'000 && tried < 20; ++number) {
        // Random names: similar ones share the upper hash bits and so fail the filter together.
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const auto name = "raw_dyn_absent_" + std::to_string(state >> 16);
        const auto hash = gnu_hash(name);
        const auto mask = (std::uint64_t{1} << (hash % 64)) | (std::uint64_t{1} << ((hash >> bloom_shift) % 64));
        if ((bloom[(hash / 64) % bloom_size] & mask) != mask || bucket[hash % buckets] == 0) continue;
        ++tried;
        CHECK(apkrt::linker::find_symbol(library->object, name) == nullptr);
    }
    CHECK(tried == 20);
}

TEST(symbols_are_found_through_the_sysv_hash_table) {
    const auto library = map_elf("libraw-dyn-sysv.so", "libraw-dyn-sysv.so");
    CHECK(library->object.dynamic.sysv_hash != nullptr && library->object.dynamic.gnu_hash == nullptr);
    check_lookup_matches_a_plain_scan(*library);
}

TEST(a_versioned_reference_finds_only_its_version) {
    if (bionic.empty()) {
        std::printf("  (bionic's libc.so is not built, see tools/build-bionic.sh)\n");
        return;
    }
    const auto libc = map_elf(bionic / "libc.so", "libc.so");
    CHECK(libc->object.dynamic.versym != nullptr);
    CHECK(apkrt::linker::find_symbol(libc->object, "memcpy", "LIBC") != nullptr);
    CHECK(apkrt::linker::find_symbol(libc->object, "memcpy", "LIBC_N") == nullptr);
    CHECK(apkrt::linker::find_symbol(libc->object, "strchrnul", "LIBC_N") != nullptr);
    CHECK(apkrt::linker::find_symbol(libc->object, "strchrnul", "LIBC") == nullptr);
    CHECK(apkrt::linker::find_symbol(libc->object, "__apkrt_start_main_thread", "APKRT_PRIVATE") != nullptr);
    CHECK(apkrt::linker::find_symbol(libc->object, "memcpy") != nullptr);  // no version asked for
    CHECK(apkrt::linker::find_symbol(libc->object, "memcpy", "LIBC_NO_SUCH_VERSION") == nullptr);

    // A version of every exported symbol, as llvm-readelf prints it (name@@VERSION), resolves.
    std::size_t checked = 0;
    const auto &dynamic = libc->object.dynamic;
    for (std::uint32_t index = 1; index < libc->symbol_count; ++index) {
        const auto &symbol = dynamic.symbols[index];
        if (!is_exported_definition(symbol)) continue;
        const auto version = apkrt::linker::symbol_version(libc->object, index);
        CHECK(!version.empty());
        CHECK(apkrt::linker::find_symbol(libc->object, dynamic.strings + symbol.name, version) == &symbol);
        ++checked;
    }
    CHECK(checked > 1400);
}

TEST(a_relocation_gets_the_first_definition_in_load_order) {
    const auto program = map_elf("raw-dyn", "raw-dyn");
    const auto library = map_elf("libraw-dyn.so", "libraw-dyn.so");
    const Object *const scope[] = {&program->object, &library->object};
    const auto address = [](const Mapped &mapped, std::string_view name) {
        return mapped.bias() + mapped.object.dynamic.symbols[symbol_index(mapped, name)].value;
    };

    // The program's definition interposes the library's own, even for the library's calls.
    CHECK(apkrt::linker::resolve(library->object, symbol_index(*library, "raw_dyn_greeting"), scope) ==
          address(*program, "raw_dyn_greeting"));
    CHECK(apkrt::linker::resolve(library->object, symbol_index(*library, "raw_dyn_counter"), scope) ==
          address(*library, "raw_dyn_counter"));
    CHECK(apkrt::linker::resolve(program->object, symbol_index(*program, "raw_dyn_print"), scope) ==
          address(*library, "raw_dyn_print"));

    // A weak reference nobody defines resolves to 0.
    CHECK(apkrt::linker::resolve(program->object, symbol_index(*program, "raw_dyn_optional"), scope) == 0);

    const Object *const program_alone[] = {&program->object};
    CHECK_THROWS(apkrt::loader::LoadError,
                 apkrt::linker::resolve(program->object, symbol_index(*program, "raw_dyn_print"), program_alone),
                 "cannot locate symbol \"raw_dyn_print\" referenced by \"raw-dyn\"");
}

/// The 16-byte TLS descriptor of the R_AARCH64_TLSDESC relocation against `name`, as relocated.
std::pair<std::uint64_t, std::uint64_t> descriptor(const Mapped &mapped, std::string_view name) {
    const auto index = symbol_index(mapped, name);
    for (const auto &entry : mapped.object.dynamic.rela) {
        if (static_cast<std::uint32_t>(entry.info) == 1031 && (entry.info >> 32) == index) {
            const auto *words = reinterpret_cast<const std::uint64_t *>(mapped.bias() + entry.offset);
            return {words[0], words[1]};
        }
    }
    throw std::runtime_error("no TLSDESC relocation against " + std::string(name));
}

TEST(tls_descriptors_point_at_the_resolvers_with_static_offsets) {
    if (!std::filesystem::exists(fixtures / "libtls-lib.so")) return;
    const auto program = map_elf("tls-test", "tls-test");
    const auto library = map_elf("libtls-lib.so", "libtls-lib.so");
    // As __apkrt_start_main_thread lays out static TLS: the program's block, then the library's.
    program->object.tls_module_id = 1;
    program->object.tls_offset = 16;
    library->object.tls_module_id = 2;
    library->object.tls_offset = 0x100;
    const Object *const scope[] = {&program->object, &library->object};
    const apkrt::linker::TlsResolvers resolvers{.static_resolver = 0x5a71c, .dynamic_resolver = 0xd1a,
                                                .unresolved_weak_resolver = 0x3ea0};
    apkrt::linker::relocate_tls(library->object, scope, resolvers);
    apkrt::linker::relocate_tls(program->object, scope, resolvers);

    const auto value = [&](std::string_view name) {
        return library->object.dynamic.symbols[symbol_index(*library, name)].value;
    };
    // Static TLS: the static resolver returns the argument, the variable's offset from the thread pointer.
    for (const char *name : {"lib_counter", "lib_zero", "lib_aligned"}) {
        CHECK(descriptor(*library, name) == std::make_pair(std::uint64_t{0x5a71c}, 0x100 + value(name)));
    }
    // An undefined weak variable: the resolver gives back the addend, so its address is NULL.
    CHECK(descriptor(*library, "lib_undefined_weak") == std::make_pair(std::uint64_t{0x3ea0}, std::uint64_t{0}));
    // The program reaches the library's variable with TPREL64: the same offset, no descriptor.
    bool found = false;
    for (const auto &entry : program->object.dynamic.rela) {
        if (static_cast<std::uint32_t>(entry.info) != 1030) continue;
        CHECK(*reinterpret_cast<const std::uint64_t *>(program->bias() + entry.offset) == 0x100 + value("lib_counter"));
        found = true;
    }
    CHECK(found);

    // Without bionic's resolvers there is nothing to point descriptors at.
    CHECK_THROWS(apkrt::loader::LoadError, apkrt::linker::relocate_tls(library->object, scope, {}),
                 "TLS descriptors need bionic's libc.so");
}

/// Objects that only have a name and DT_NEEDED, for the order tests.
Object fake(const std::string &name, std::vector<std::string> needed) {
    Object object{.name = name, .image = {}, .dynamic = {}};
    object.dynamic.needed = std::move(needed);
    return object;
}

std::vector<std::string> names(const std::vector<Object> &objects) {
    std::vector<std::string> result;
    for (const auto &object : objects) result.push_back(object.name);
    return result;
}

TEST(libraries_load_breadth_first_and_once) {
    // program needs a and b; a needs c; b needs c and a.
    const std::map<std::string, std::vector<std::string>> needed = {{"a", {"c"}}, {"b", {"c", "a"}}, {"c", {}}};
    std::vector<std::string> opened;
    const auto objects = apkrt::linker::load_all(fake("program", {"a", "b"}), [&](const std::string &name) {
        opened.push_back(name);
        return fake(name, needed.at(name));
    });
    CHECK(names(objects) == (std::vector<std::string>{"program", "a", "b", "c"}));
    CHECK(opened == (std::vector<std::string>{"a", "b", "c"}));
}

TEST(constructors_run_after_the_libraries_they_need) {
    const std::vector<Object> objects = {fake("program", {"a", "b"}), fake("a", {"c"}), fake("b", {"c", "a"}),
                                         fake("c", {})};
    std::vector<const Object *> pointers;
    for (const auto &object : objects) pointers.push_back(&object);
    std::vector<std::string> order;
    for (const auto *object : apkrt::linker::initialization_order(pointers)) order.push_back(object->name);
    CHECK(order == (std::vector<std::string>{"c", "a", "b", "program"}));

    // For a library dlopen asks for: only the libraries loaded with it, whose constructors have not run.
    const std::vector<const Object *> new_ones = {&objects[2], &objects[3]};  // b, c; a is there already
    order.clear();
    for (const auto *object : apkrt::linker::initialization_order(new_ones)) order.push_back(object->name);
    CHECK(order == (std::vector<std::string>{"c", "b"}));
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: linker_test <fixtures-directory> [<bionic-library-directory>]\n");
        return 2;
    }
    fixtures = argv[1];
    if (argc == 3 && std::filesystem::exists(std::filesystem::path(argv[2]) / "libc.so")) {
        bionic = std::filesystem::canonical(argv[2]);
    }
    for (const char *name : {"raw-dyn", "libraw-dyn.so", "libraw-dyn-sysv.so"}) {
        if (!std::filesystem::exists(fixtures / name)) {
            std::printf("skipped: %s is not built, see tools/build-fixtures.sh\n", name);
            return 77;
        }
    }
    return apk::testing::run_all();
}
