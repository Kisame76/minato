// hello: one line through printf from the static NDK libc.
// Expected output: "hello from hello"
#include <stdio.h>

int main(void) {
    printf("hello from hello\n");
    return 0;
}
