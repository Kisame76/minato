// hello-dyn: one line through printf from bionic's libc.so, which the runtime builds from AOSP
// sources and loads like any library (M2 step 5b, ADR-0021).
// Expected output: "hello from hello-dyn"
#include <stdio.h>

int main(void) {
    printf("hello from hello-dyn\n");
    return 0;
}
