BITS 64
DEFAULT REL

SECTION .text

GLOBAL idt_load
GLOBAL idt_default_handler

; void idt_load(idt_pointer_t *pointer)
; RDI = IDT descriptor

idt_load:
    lidt [rdi]
    ret


; Temporary handler for all 256 vectors.
; Later i will replace this with proper ISR handlers.

idt_default_handler:
    cli

.halt:
    hlt
    jmp .halt

SECTION .note.GNU-stack noalloc noexec nowrite progbits