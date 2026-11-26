BITS 64
DEFAULT REL

SECTION .text

EXTERN syscall_dispatch
GLOBAL syscall_stub

syscall_stub:
    cld

    push qword 0
    push qword 0x80

    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    and rsp, -16
    call syscall_dispatch
    mov rsp, rax

    test byte [rsp + 144], 3
    jz .kernel_segments
    mov ax, 0x1B
    jmp .load_segments

.kernel_segments:
    mov ax, 0x10

.load_segments:
    mov ds, ax
    mov es, ax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    iretq

SECTION .note.GNU-stack noalloc noexec nowrite progbits
