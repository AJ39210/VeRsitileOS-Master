BITS 64
DEFAULT REL

SECTION .text

EXTERN irq_dispatch
GLOBAL irq_resume_frame

%macro IRQ_STUB 1
GLOBAL irq%1_stub

irq%1_stub:
    push qword 0
    push qword (32 + %1)
    jmp irq_common_stub
%endmacro

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

irq_common_stub:
    cld

    ; Save general-purpose registers.
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

    ; Pass the saved register/interrupt frame to the dispatcher.
    mov rdi, rsp

    ; Preserve the original stack pointer.
    mov r12, rsp

    ; Align stack for the C function call.
    and rsp, -16

    call irq_dispatch

    ; The dispatcher may select a different task's interrupt frame.
    mov rsp, rax
    jmp irq_restore_frame

irq_resume_frame:
    mov rsp, rdi

irq_restore_frame:
    test byte [rsp + 144], 3
    jz .kernel_segments
    mov ax, 0x1B
    jmp .load_segments

.kernel_segments:
    mov ax, 0x10

.load_segments:
    mov ds, ax
    mov es, ax

    ; Restore registers.
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

    ; Remove vector and dummy error code.
    add rsp, 16

    iretq

SECTION .note.GNU-stack noalloc noexec nowrite progbits