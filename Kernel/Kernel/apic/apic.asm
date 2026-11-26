BITS 64

SECTION .text
GLOBAL apic_spurious_stub

apic_spurious_stub:
    ; A spurious APIC interrupt does not receive an EOI.
    iretq

SECTION .note.GNU-stack noalloc noexec nowrite progbits