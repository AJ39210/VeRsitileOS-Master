
BITS 64
DEFAULT REL

SECTION .text

GLOBAL gdt_load
GLOBAL tss_load

; void gdt_load(gdt_pointer_t *pointer)
; RDI = pointer to GDT descriptor

gdt_load:
    lgdt [rdi]

    ; Reload data segment registers.
    mov ax, 0x10

    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Reload CS using a far return.
    push qword 0x08

    lea rax, [rel .reload_cs]
    push rax

    retfq

.reload_cs:
    ret


; void tss_load(uint16_t selector)
; DI = TSS selector

tss_load:
    mov ax, di
    ltr ax
    ret

SECTION .note.GNU-stack noalloc noexec nowrite progbits
