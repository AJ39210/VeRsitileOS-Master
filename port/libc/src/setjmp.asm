BITS 64
DEFAULT REL

SECTION .text
GLOBAL setjmp:function (setjmp_end - setjmp)
GLOBAL longjmp:function (longjmp_end - longjmp)

setjmp:
    mov [rdi + 0], rbx
    mov [rdi + 8], rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rax, [rsp + 8]
    mov [rdi + 48], rax
    mov rax, [rsp]
    mov [rdi + 56], rax
    xor eax, eax
    ret
setjmp_end:

longjmp:
    mov rax, rsi
    test rax, rax
    jnz .value_ready
    mov eax, 1
.value_ready:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    jmp [rdi + 56]
longjmp_end:

SECTION .note.GNU-stack noalloc noexec nowrite progbits
