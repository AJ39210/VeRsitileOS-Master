#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>
#include <stdbool.h>

#define IRQ_BASE 32
#define IRQ_COUNT 16

typedef struct
{
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t user_rsp;
    uint64_t user_ss;
} irq_frame_t;

typedef void (*irq_handler_t)(void);

void irq_init(void);

bool irq_register_handler(uint8_t irq, irq_handler_t handler);
uint64_t irq_get_count(uint8_t irq);
irq_frame_t *irq_dispatch(irq_frame_t *frame);
void irq_resume_frame(irq_frame_t *frame)
    __attribute__((noreturn));

#endif