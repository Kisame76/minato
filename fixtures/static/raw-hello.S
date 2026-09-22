// raw-hello: writes one line with Linux system calls and exits with status 0.
// Expected output: "hello from raw-hello"

    .section .rodata
message:
    .ascii  "hello from raw-hello\n"
    .set    message_size, . - message

    .text
    .globl  _start
    .type   _start, %function
_start:
    mov     x0, #1                  // stdout
    adrp    x1, message
    add     x1, x1, :lo12:message
    mov     x2, #message_size
    mov     x8, #64                 // __NR_write
    svc     #0
    mov     x0, #0                  // exit status
    mov     x8, #94                 // __NR_exit_group
    svc     #0
    .size   _start, . - _start

    .section .note.GNU-stack, "", %progbits
