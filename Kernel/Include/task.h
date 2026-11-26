#ifndef TASK_H
#define TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "irq.h"

typedef struct
{
    uint64_t pid;
    uint64_t parent_pid;
    char state[12];
} task_info_t;

bool tasking_init(void);
void tasking_start(void) __attribute__((noreturn));

irq_frame_t *task_schedule_from_interrupt(irq_frame_t *frame);
irq_frame_t *task_schedule_from_syscall(
    irq_frame_t *frame,
    bool terminate
);

uint64_t task_current_pid(void);
uint32_t task_count(void);
bool task_get_info(uint32_t index, task_info_t *info);
bool task_user_range_valid(
    uint64_t address,
    uint64_t size,
    bool writable
);
bool task_copy_from_user(void *destination, uint64_t source, uint64_t size);
bool task_copy_to_user(uint64_t destination, const void *source, uint64_t size);
uint64_t task_allocate_user(uint64_t size);
bool task_free_user(uint64_t address);
int64_t task_map_user(
    uint64_t hint,
    uint64_t size,
    uint64_t protection
);
int64_t task_unmap_user(uint64_t address, uint64_t size);
int64_t task_protect_user(
    uint64_t address,
    uint64_t size,
    uint64_t protection
);
int64_t task_open_file(const char *path, int flags);
int64_t task_read_file(int64_t descriptor, uint64_t buffer, uint64_t size);
int64_t task_write_file(
    int64_t descriptor,
    const void *buffer,
    uint64_t size
);
int64_t task_seek_file(int64_t descriptor, int64_t offset, int whence);
int64_t task_close_file(int64_t descriptor);
int64_t task_socketpair(int domain, int type, int protocol, int descriptors[2]);
int64_t task_socket_readiness(int64_t descriptor, short events, short *revents);
int64_t task_spawn_user(uint64_t entry);
int64_t task_spawn_elf(const char *path);
int64_t task_spawn_elf_command(const char *path, const char *argument);
int64_t task_write_parent_output(const void *buffer, uint64_t size);
int64_t task_read_child_output(uint64_t buffer, uint64_t size);
bool task_pid_is_active(uint64_t pid);

#endif
