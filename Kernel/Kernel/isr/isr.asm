
BITS 64
DEFAULT REL

SECTION .text

EXTERN isr_dispatch

%macro ISR_NO_ERROR 1
GLOBAL isr%1_stub

isr%1_stub:
    push qword 0
    push qword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERROR 1
GLOBAL isr%1_stub

isr%1_stub:
    ; CPU already pushed the error code.
    push qword %1
    jmp isr_common_stub
%endmacro

; Exceptions without hardware error codes.

ISR_NO_ERROR 0
ISR_NO_ERROR 1
ISR_NO_ERROR 2
ISR_NO_ERROR 3
ISR_NO_ERROR 4
ISR_NO_ERROR 5
ISR_NO_ERROR 6
ISR_NO_ERROR 7

; Exceptions with hardware error codes.

ISR_ERROR 8
ISR_ERROR 10
ISR_ERROR 11
ISR_ERROR 12
ISR_ERROR 13
ISR_ERROR 14

ISR_NO_ERROR 9

ISR_ERROR 17

ISR_NO_ERROR 15
ISR_NO_ERROR 16

; Modern CPU exceptions.

ISR_ERROR 21
ISR_ERROR 29
ISR_ERROR 30

; Remaining reserved exception vectors.

ISR_NO_ERROR 18
ISR_NO_ERROR 19
ISR_NO_ERROR 20
ISR_NO_ERROR 22
ISR_NO_ERROR 23
ISR_NO_ERROR 24
ISR_NO_ERROR 25
ISR_NO_ERROR 26
ISR_NO_ERROR 27
ISR_NO_ERROR 28
ISR_NO_ERROR 31


; Common exception handler.

isr_common_stub:
    cld

    ; Save every general-purpose register.

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

    ; RDI = pointer to isr_frame_t.

    mov rdi, rsp

    ; Preserve the original interrupt stack.

    mov r12, rsp

    ; Align stack for the C ABI.

    and rsp, -16

    call isr_dispatch

    ; isr_dispatch must never return.

.halt:
    cli
    hlt
    jmp .halt


; Table of all 32 exception stubs.

SECTION .rodata

GLOBAL isr_stub_table

isr_stub_table:
    dq isr0_stub
    dq isr1_stub
    dq isr2_stub
    dq isr3_stub
    dq isr4_stub
    dq isr5_stub
    dq isr6_stub
    dq isr7_stub
    dq isr8_stub
    dq isr9_stub
    dq isr10_stub
    dq isr11_stub
    dq isr12_stub
    dq isr13_stub
    dq isr14_stub
    dq isr15_stub
    dq isr16_stub
    dq isr17_stub
    dq isr18_stub
    dq isr19_stub
    dq isr20_stub
    dq isr21_stub
    dq isr22_stub
    dq isr23_stub
    dq isr24_stub
    dq isr25_stub
    dq isr26_stub
    dq isr27_stub
    dq isr28_stub
    dq isr29_stub
    dq isr30_stub
    dq isr31_stub

SECTION .note.GNU-stack noalloc noexec nowrite progbits
