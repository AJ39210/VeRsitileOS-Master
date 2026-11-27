BITS 64
DEFAULT REL

SECTION .text
GLOBAL _start
EXTERN main
EXTERN exit

_start:
    pop rdi
    mov rsi, rsp
    and rsp, -16
    call main wrt ..plt
    mov edi, eax
    call exit wrt ..plt

.halt:
    pause
    jmp .halt

SECTION .note.GNU-stack noalloc noexec nowrite progbits
