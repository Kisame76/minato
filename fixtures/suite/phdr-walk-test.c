// phdr-walk-test: the module walk a crash reporter or an engine does at its start: dl_iterate_phdr, and for each
// module its dynamic section (PT_DYNAMIC at dlpi_addr + p_vaddr), its name through DT_STRTAB + DT_SONAME (Android's
// linker leaves d_ptr at its link-time value, so the address is dlpi_addr + d_ptr), its ELF header at the address of
// the segment that maps file offset 0, and its GNU build ID (PT_NOTE). A module whose dlpi_addr or d_ptr is wrong
// makes the walk read NULL + a small odd offset, as a string byte is. It prints only what both systems share: its own
// library (libphdr-walk.so) and libc.so, whether every module's name could be read, and whether dladdr's base of
// its own library is the walk's.
// Expected output: fixtures/expected/phdr-walk-test.out (Android 15's).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int phdr_walk_marker(void);

struct walk {
    int modules;
    int named;               // modules with a DT_SONAME
    int own_found, libc_found;
    int own_header;          // the ELF header at the own library's address
    unsigned own_build_id;   // the own library's build ID length, 0 without one
    ElfW(Addr) own_addr;
};

static const char *soname_of(const struct dl_phdr_info *info) {
    for (int index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr) *header = &info->dlpi_phdr[index];
        if (header->p_type != PT_DYNAMIC) continue;
        const ElfW(Dyn) *entry = (const ElfW(Dyn) *)(info->dlpi_addr + header->p_vaddr);
        ElfW(Addr) strtab = 0;
        ElfW(Xword) soname = 0;
        int has_soname = 0;
        for (; entry->d_tag != DT_NULL; ++entry) {
            if (entry->d_tag == DT_STRTAB) strtab = entry->d_un.d_ptr;
            if (entry->d_tag == DT_SONAME) {
                soname = entry->d_un.d_val;
                has_soname = 1;
            }
        }
        if (!has_soname || strtab == 0) return NULL;
        return (const char *)(info->dlpi_addr + strtab + soname);
    }
    return NULL;
}

static unsigned build_id_of(const struct dl_phdr_info *info) {
    for (int index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr) *header = &info->dlpi_phdr[index];
        if (header->p_type != PT_NOTE) continue;
        const unsigned char *at = (const unsigned char *)(info->dlpi_addr + header->p_vaddr);
        const unsigned char *end = at + header->p_memsz;
        while (at + sizeof(ElfW(Nhdr)) <= end) {
            const ElfW(Nhdr) *note = (const ElfW(Nhdr) *)at;
            const unsigned char *name = at + sizeof *note;
            const unsigned char *desc = name + ((note->n_namesz + 3u) & ~3u);
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && memcmp(name, "GNU", 4) == 0) {
                return note->n_descsz;
            }
            at = desc + ((note->n_descsz + 3u) & ~3u);
        }
    }
    return 0;
}

static int visit(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    struct walk *walk = data;
    ++walk->modules;
    const char *name = soname_of(info);
    if (name == NULL) return 0;
    ++walk->named;
    if (strcmp(name, "libphdr-walk.so") == 0) {
        walk->own_found = 1;
        walk->own_addr = info->dlpi_addr;
        for (int index = 0; index < info->dlpi_phnum; ++index) {
            const ElfW(Phdr) *header = &info->dlpi_phdr[index];
            if (header->p_type == PT_LOAD && header->p_offset == 0) {
                walk->own_header = memcmp((const void *)(info->dlpi_addr + header->p_vaddr), ELFMAG, SELFMAG) == 0;
            }
        }
        walk->own_build_id = build_id_of(info);
    } else if (strcmp(name, "libc.so") == 0) {
        walk->libc_found = 1;
    }
    return 0;
}

int main(void) {
    struct walk walk = {0};
    dl_iterate_phdr(visit, &walk);
    printf("modules: %s\n", walk.modules >= 3 ? "3 or more" : "fewer than 3");
    printf("named modules read: %s\n", walk.named >= 2 ? "2 or more" : "fewer than 2");
    printf("libphdr-walk.so: %s\n", walk.own_found ? "found by its DT_SONAME" : "missing");
    printf("libc.so: %s\n", walk.libc_found ? "found by its DT_SONAME" : "missing");
    printf("ELF header at the own library's first segment: %s\n", walk.own_header ? "yes" : "no");
    printf("build ID: %u bytes\n", walk.own_build_id);
    Dl_info found = {0};
    const int known = dladdr((const void *)phdr_walk_marker, &found);
    printf("dladdr base is dlpi_addr: %s\n",
           known != 0 && (ElfW(Addr))found.dli_fbase == walk.own_addr ? "yes" : "no");
    printf("marker: %d\n", phdr_walk_marker());
    return 0;
}
