// ffi-closure-test: an app that makes closures the way ctypes does (libffi closures without executable memory,
// ADR-0072). Without an argument it imports the three libffi names from libfake-ffi.so and calls them itself; with
// "symbolic" it opens libfake-ffi-sym.so with dlopen and uses its fake_ffi_make / fake_ffi_release, which call the
// names from inside the library by direct branches. Expected output: fixtures/expected/ffi-closure-test.out, the same
// for both.
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

void *ffi_closure_alloc(size_t size, void **code);
void ffi_closure_free(void *closure);
int ffi_prep_closure_loc(void *closure, void *cif, void *fun, void *user_data, void *codeloc);
void fake_ffi_fun(void *cif, long *ret, long *arg, void *user_data);
static void *(*fake_ffi_make)(long user);
static void (*fake_ffi_release)(void *code);

static int symbolic;
static void *writable_of[4];
static void *code_of[4];

static void *make(long user) {
    if (symbolic) return fake_ffi_make(user);
    void *code = NULL;
    void *writable = ffi_closure_alloc(0x30, &code);
    if (writable == NULL) return NULL;
    if (ffi_prep_closure_loc(writable, NULL, (void *)fake_ffi_fun, (void *)user, code) != 0) return NULL;
    for (int index = 0; index < 4; index++) {
        if (code_of[index] == NULL) {
            code_of[index] = code;
            writable_of[index] = writable;
            break;
        }
    }
    return code;
}

static void release(void *code) {
    if (symbolic) {
        fake_ffi_release(code);
        return;
    }
    for (int index = 0; index < 4; index++) {
        if (code_of[index] == code) {
            ffi_closure_free(writable_of[index]);
            code_of[index] = NULL;
            return;
        }
    }
}

int main(int argc, char **argv) {
    symbolic = argc > 1 && strcmp(argv[1], "symbolic") == 0;
    if (symbolic) {
        void *library = dlopen("libfake-ffi-sym.so", RTLD_NOW);
        if (library != NULL) {
            fake_ffi_make = (void *(*)(long))dlsym(library, "fake_ffi_make");
            fake_ffi_release = (void (*)(void *))dlsym(library, "fake_ffi_release");
        }
        if (fake_ffi_make == NULL || fake_ffi_release == NULL) {
            printf("ffi-closure-test: %s\n", dlerror());
            return 1;
        }
    }
    static const long users[] = {7, 100, 1000};
    void *codes[3];
    for (int index = 0; index < 3; index++) {
        codes[index] = make(users[index]);
        if (codes[index] == NULL) {
            printf("ffi-closure-test: closure %d: no executable memory\n", index);
            return 1;
        }
        printf("ffi-closure-test: closure %d: f(5) = %ld\n", index, ((long (*)(long))codes[index])(5));
    }
    printf("ffi-closure-test: code pointers differ: %s\n",
           codes[0] != codes[1] && codes[1] != codes[2] && codes[0] != codes[2] ? "yes" : "no");
    release(codes[1]);
    void *again = make(5000);
    printf("ffi-closure-test: freed closure 1, new closure: f(5) = %ld\n",
           again != NULL ? ((long (*)(long))again)(5) : -1L);
    printf("ffi-closure-test: closure 0 still: f(6) = %ld\n", ((long (*)(long))codes[0])(6));
    release(codes[0]);
    release(codes[2]);
    release(again);
    printf("ffi-closure-test: freed all\n");
    return 0;
}
