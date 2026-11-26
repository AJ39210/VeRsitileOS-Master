#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gdt.h"
#include "elf_loader.h"
#include "heap.h"
#include "paging.h"
#include "pmm.h"
#include "syscall.h"
#include "task.h"
#include "vfs.h"
#include "vmm.h"
#include <sys/socket.h>

#define TASK_LIMIT 32
#define TASK_KERNEL_STACK_SIZE (16 * VMM_PAGE_SIZE)
#define TASK_USER_STACK_PAGES 4

#define USER_STACK_BASE 0x7FFFC000ULL
#define USER_STACK_TOP  (USER_STACK_BASE + TASK_USER_STACK_PAGES * VMM_PAGE_SIZE)
#define USER_ALLOC_BASE 0x10000000ULL
#define USER_ALLOC_LIMIT 0x20000000ULL
#define TASK_MAX_ALLOCS 64
#define TASK_MAX_FILES  16
#define TASK_MAX_LOCAL_CONNECTIONS 32
#define TASK_SOCKET_BUFFER_SIZE 4096
#define TASK_CHILD_OUTPUT_SIZE 16384
#define TASK_COMMAND_ARGUMENT_MAX 255

enum
{
    TASK_POLLIN = 0x001,
    TASK_POLLOUT = 0x004,
    TASK_POLLERR = 0x008,
    TASK_POLLHUP = 0x010
};

typedef struct
{
    bool used;
    uint8_t open_ends;
    uint32_t read_offset[2];
    uint32_t used_bytes[2];
    uint8_t data[2][TASK_SOCKET_BUFFER_SIZE];
} task_local_connection_t;

typedef enum
{
    TASK_UNUSED,
    TASK_READY,
    TASK_RUNNING,
    TASK_IDLE,
    TASK_ZOMBIE
} task_state_t;

typedef struct
{
    bool used;
    bool is_socket;
    bool readable;
    bool writable;
    bool append;
    uint8_t socket_side;
    uint8_t socket_connection;
    vfs_file_t file;
} task_file_t;

typedef struct
{
    uint64_t address;
    uint64_t pages;
} task_allocation_t;

typedef struct
{
    uint64_t pid;
    uint64_t parent_pid;
    task_state_t state;
    vmm_address_space_t address_space;
    void *kernel_stack;
    uint64_t kernel_stack_top;
    irq_frame_t *frame;
    task_allocation_t allocations[TASK_MAX_ALLOCS];
    task_file_t files[TASK_MAX_FILES];
    bool capture_output;
    uint32_t child_output_read;
    uint32_t child_output_write;
    uint32_t child_output_used;
    char child_output[TASK_CHILD_OUTPUT_SIZE];
} task_t;

static task_t tasks[TASK_LIMIT];
static task_t *current_task;
static task_t *idle_task;
static uint64_t next_pid = 1;
static bool tasking_ready;
static task_local_connection_t local_connections[
    TASK_MAX_LOCAL_CONNECTIONS
];

static void bytes_zero(void *pointer, uint64_t size)
{
    uint8_t *bytes = pointer;
    for (uint64_t i = 0; i < size; i++)
        bytes[i] = 0;
}

static void bytes_copy(void *destination, const void *source, uint64_t size)
{
    uint8_t *dst = destination;
    const uint8_t *src = source;
    for (uint64_t i = 0; i < size; i++)
        dst[i] = src[i];
}

static void release_local_socket(task_file_t *file)
{
    if (!file || !file->used || !file->is_socket ||
        file->socket_connection >= TASK_MAX_LOCAL_CONNECTIONS)
        return;

    task_local_connection_t *connection =
        &local_connections[file->socket_connection];
    uint8_t end_mask = (uint8_t)(1U << file->socket_side);
    connection->open_ends &= (uint8_t)~end_mask;
    if (!connection->open_ends)
        bytes_zero(connection, sizeof(*connection));
}

static int64_t socket_read_file(
    task_file_t *file,
    uint64_t buffer,
    uint64_t size
)
{
    task_local_connection_t *connection =
        &local_connections[file->socket_connection];
    uint8_t incoming = (uint8_t)(1U - file->socket_side);
    uint32_t available = connection->used_bytes[incoming];
    if (!available)
        return connection->open_ends & (1U << incoming)
            ? -SYSCALL_EAGAIN
            : 0;

    uint64_t amount = size < available ? size : available;
    uint32_t offset = connection->read_offset[incoming];
    uint32_t first = TASK_SOCKET_BUFFER_SIZE - offset;
    if (first > amount)
        first = (uint32_t)amount;
    if (!task_copy_to_user(
            buffer,
            connection->data[incoming] + offset,
            first))
        return -SYSCALL_EFAULT;
    if (amount > first &&
        !task_copy_to_user(
            buffer + first,
            connection->data[incoming],
            amount - first))
        return -SYSCALL_EFAULT;

    connection->read_offset[incoming] =
        (offset + (uint32_t)amount) % TASK_SOCKET_BUFFER_SIZE;
    connection->used_bytes[incoming] -= (uint32_t)amount;
    return (int64_t)amount;
}

static int64_t socket_write_file(
    task_file_t *file,
    const void *buffer,
    uint64_t size
)
{
    task_local_connection_t *connection =
        &local_connections[file->socket_connection];
    uint8_t outgoing = file->socket_side;
    uint8_t peer_mask = (uint8_t)(1U << (1U - outgoing));
    if (!(connection->open_ends & peer_mask))
        return -SYSCALL_EPIPE;

    uint32_t available =
        TASK_SOCKET_BUFFER_SIZE - connection->used_bytes[outgoing];
    if (!available)
        return -SYSCALL_EAGAIN;
    uint64_t amount = size < available ? size : available;

    uint32_t write_offset =
        (connection->read_offset[outgoing] +
         connection->used_bytes[outgoing]) %
        TASK_SOCKET_BUFFER_SIZE;
    uint32_t first = TASK_SOCKET_BUFFER_SIZE - write_offset;
    if (first > amount)
        first = (uint32_t)amount;
    bytes_copy(
        connection->data[outgoing] + write_offset,
        buffer,
        first);
    if (amount > first)
        bytes_copy(
            connection->data[outgoing],
            (const uint8_t *)buffer + first,
            amount - first);
    connection->used_bytes[outgoing] += (uint32_t)amount;
    return (int64_t)amount;
}

static task_t *allocate_task_slot(void)
{
    for (uint32_t i = 0; i < TASK_LIMIT; i++)
    {
        if (tasks[i].state == TASK_UNUSED)
        {
            bytes_zero(&tasks[i], sizeof(tasks[i]));
            return &tasks[i];
        }
    }

    return NULL;
}

static bool allocate_kernel_stack(task_t *task)
{
    task->kernel_stack = kmemalign(
        VMM_PAGE_SIZE,
        TASK_KERNEL_STACK_SIZE
    );

    if (!task->kernel_stack)
        return false;

    task->kernel_stack_top =
        (uint64_t)(uintptr_t)task->kernel_stack +
        TASK_KERNEL_STACK_SIZE;

    return true;
}

static uint64_t allocate_user_page(
    vmm_address_space_t address_space,
    uint64_t virtual_address,
    uint64_t protection
)
{
    uint64_t physical = pmm_alloc_page();
    if (!physical)
        return 0;

    uint64_t flags = VMM_PRESENT | VMM_USER;
    if (protection & 0x2)
        flags |= VMM_WRITABLE;
    if (!(protection & 0x4) && paging_nx_supported())
        flags |= VMM_NO_EXECUTE;

    if (!vmm_map_page_in(
            address_space,
            virtual_address,
            physical,
            flags))
    {
        pmm_free_page(physical);
        return 0;
    }

    void *page = paging_phys_to_virt(physical);
    if (!page)
    {
        vmm_unmap_page_in(address_space, virtual_address);
        pmm_free_page(physical);
        return 0;
    }

    bytes_zero(page, VMM_PAGE_SIZE);
    return physical;
}

static bool map_user_stack(
    vmm_address_space_t address_space,
    bool replace_existing
)
{
    for (uint64_t i = 0; i < TASK_USER_STACK_PAGES; i++)
    {
        uint64_t virtual_address =
            USER_STACK_BASE + i * VMM_PAGE_SIZE;

        if (replace_existing)
        {
            uint64_t old_physical =
                vmm_translate_in(address_space, virtual_address);

            if (!old_physical ||
                !vmm_unmap_page_in(address_space, virtual_address) ||
                !pmm_free_page(old_physical))
            {
                return false;
            }
        }

        if (!allocate_user_page(address_space, virtual_address, 0x3))
            return false;
    }

    return true;
}

static irq_frame_t *make_user_frame(
    task_t *task,
    uint64_t entry,
    uint64_t initial_argument
)
{
    irq_frame_t *frame =
        (irq_frame_t *)(uintptr_t)(
            task->kernel_stack_top - sizeof(irq_frame_t)
        );

    bytes_zero(frame, sizeof(*frame));
    frame->rdi = initial_argument;
    frame->rip = entry;
    frame->cs = GDT_USER_CODE | 3;
    frame->rflags = 0x202;
    frame->user_rsp = USER_STACK_TOP - 16;
    frame->user_ss = GDT_USER_DATA | 3;
    return frame;
}

static bool write_address_space(
    vmm_address_space_t address_space,
    uint64_t destination,
    const void *source,
    uint64_t size
)
{
    const uint8_t *input = source;
    while (size)
    {
        uint64_t physical = vmm_translate_in(address_space, destination);
        if (!physical)
            return false;
        uint64_t amount = VMM_PAGE_SIZE -
            (destination & (VMM_PAGE_SIZE - 1));
        if (amount > size)
            amount = size;
        uint8_t *output = paging_phys_to_virt(physical);
        if (!output)
            return false;
        bytes_copy(output, input, amount);
        destination += amount;
        input += amount;
        size -= amount;
    }
    return true;
}

static bool prepare_command_stack(
    task_t *task,
    const char *path,
    const char *argument,
    uint64_t *stack_pointer
)
{
    uint64_t path_length = 0;
    while (path[path_length])
        path_length++;
    uint64_t argument_length = 0;
    if (argument)
        while (argument[argument_length])
            argument_length++;
    if (!path_length || path_length >= 256 ||
        argument_length > TASK_COMMAND_ARGUMENT_MAX)
        return false;

    uint64_t sp = USER_STACK_TOP;
    uint64_t path_address = sp - path_length - 1;
    if (!write_address_space(
            task->address_space,
            path_address,
            path,
            path_length + 1))
        return false;
    sp = path_address;

    uint64_t argument_address = 0;
    uint64_t argc = argument_length ? 2 : 1;
    if (argument_length)
    {
        argument_address = sp - argument_length - 1;
        if (!write_address_space(
                task->address_space,
                argument_address,
                argument,
                argument_length + 1))
            return false;
        sp = argument_address;
    }

    uint64_t words[5] = {
        argc,
        path_address,
        argument_address,
        0,
        0
    };
    uint64_t word_count = argc + 3;
    sp = (sp - word_count * sizeof(uint64_t)) & ~0xFULL;
    if (!write_address_space(
            task->address_space,
            sp,
            words,
            word_count * sizeof(uint64_t)))
        return false;
    *stack_pointer = sp;
    return true;
}

static void idle_loop(void) __attribute__((noreturn));

static void idle_loop(void)
{
    for (;;)
        __asm__ volatile ("sti; hlt" ::: "memory");
}

static bool initialize_idle_task(void)
{
    task_t *task = allocate_task_slot();
    if (!task || !allocate_kernel_stack(task))
        return false;

    task->pid = 0;
    task->state = TASK_IDLE;
    task->address_space = vmm_current_address_space();
    task->frame = NULL;
    idle_task = task;
    return true;
}

static bool initialize_first_user_task(void)
{
    task_t *task = allocate_task_slot();
    if (!task)
        return false;

    task->address_space = vmm_create_address_space();
    if (!task->address_space)
        return false;

    if (!allocate_kernel_stack(task))
    {
        vmm_destroy_address_space(task->address_space);
        bytes_zero(task, sizeof(*task));
        return false;
    }

    uint64_t entry;
    if (elf_load_executable(
            task->address_space,
            "/init/systemd",
            &entry) < 0)
        goto fail;

    if (!map_user_stack(task->address_space, false))
        goto fail;

    uint64_t user_stack_pointer;
    if (!prepare_command_stack(
            task,
            "/init/systemd",
            "/etc/system/systemd/main.cfg",
            &user_stack_pointer))
        goto fail;

    task->pid = next_pid++;
    task->parent_pid = 0;
    task->state = TASK_READY;
    task->frame = make_user_frame(task, entry, 0);
    task->frame->user_rsp = user_stack_pointer;
    return true;

fail:
    if (task->address_space != 0)
        vmm_destroy_address_space(task->address_space);
    if (task->kernel_stack)
        kfree(task->kernel_stack);
    bytes_zero(task, sizeof(*task));
    return false;
}

bool tasking_init(void)
{
    if (tasking_ready)
        return true;

    bytes_zero(tasks, sizeof(tasks));
    bytes_zero(local_connections, sizeof(local_connections));
    current_task = NULL;
    idle_task = NULL;
    next_pid = 1;

    if (!initialize_idle_task() ||
        !initialize_first_user_task())
    {
        if (idle_task && idle_task->kernel_stack)
            kfree(idle_task->kernel_stack);
        bytes_zero(tasks, sizeof(tasks));
        idle_task = NULL;
        return false;
    }

    tasking_ready = true;
    return true;
}

static void reap_zombie_tasks(void)
{
    for (uint32_t i = 0; i < TASK_LIMIT; i++)
    {
        task_t *task = &tasks[i];

        if (task == current_task ||
            task->state != TASK_ZOMBIE)
            continue;

        if (task->address_space)
            vmm_destroy_address_space(task->address_space);

        for (uint32_t j = 0; j < TASK_MAX_FILES; j++)
        {
            if (task->files[j].used && !task->files[j].is_socket)
                vfs_close_file(&task->files[j].file);
            release_local_socket(&task->files[j]);
        }

        if (task->kernel_stack)
            kfree(task->kernel_stack);

        bytes_zero(task, sizeof(*task));
    }
}

static task_t *next_ready_task(task_t *after)
{
    uint32_t start = 0;
    if (after)
        start = (uint32_t)(after - tasks);

    for (uint32_t step = 1; step <= TASK_LIMIT; step++)
    {
        uint32_t index = (start + step) % TASK_LIMIT;
        if (tasks[index].state == TASK_READY)
            return &tasks[index];
    }

    return idle_task;
}

static irq_frame_t *schedule(
    irq_frame_t *frame,
    bool terminate
)
{
    reap_zombie_tasks();

    task_t *previous = current_task;

    if (previous && previous != idle_task)
    {
        if (terminate)
        {
            previous->state = TASK_ZOMBIE;
            previous->frame = NULL;
        }
        else
        {
            previous->frame = frame;
            previous->state = TASK_READY;
        }
    }
    else if (previous == idle_task && frame)
    {
        previous->frame = frame;
    }

    task_t *next = next_ready_task(previous);

    if (!next)
        next = idle_task;

    if (!terminate && next == previous)
    {
        previous->state =
            previous == idle_task ? TASK_IDLE : TASK_RUNNING;
        return frame;
    }

    if (!terminate && previous != idle_task &&
        next == idle_task)
    {
        previous->state = TASK_RUNNING;
        return frame;
    }

    if (next == previous && terminate)
        next = idle_task;

    if (!next || !next->frame)
        return frame;

    current_task = next;
    next->state =
        next == idle_task ? TASK_IDLE : TASK_RUNNING;

    if (!vmm_switch_address_space(next->address_space))
        return frame;

    gdt_set_kernel_stack(next->kernel_stack_top);
    return next->frame;
}

irq_frame_t *task_schedule_from_interrupt(irq_frame_t *frame)
{
    if (!tasking_ready || !frame)
        return frame;

    if ((frame->cs & 3) != 3 && current_task != idle_task)
        return frame;

    return schedule(frame, false);
}

irq_frame_t *task_schedule_from_syscall(
    irq_frame_t *frame,
    bool terminate
)
{
    if (!tasking_ready || !frame ||
        (frame->cs & 3) != 3 ||
        !current_task || current_task == idle_task)
        return frame;

    return schedule(frame, terminate);
}

void tasking_start(void)
{
    if (!tasking_ready || !idle_task)
        for (;;)
            __asm__ volatile ("cli; hlt");

    current_task = idle_task;
    gdt_set_kernel_stack(idle_task->kernel_stack_top);
    idle_loop();
}

uint64_t task_current_pid(void)
{
    if (!current_task || current_task == idle_task)
        return 0;
    return current_task->pid;
}

static const char *task_state_name(task_state_t state)
{
    switch (state)
    {
        case TASK_READY: return "Ready";
        case TASK_RUNNING: return "Running";
        case TASK_IDLE: return "Idle";
        case TASK_ZOMBIE: return "Zombie";
        default: return "Unused";
    }
}

uint32_t task_count(void)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < TASK_LIMIT; i++)
    {
        if (tasks[i].state == TASK_READY ||
            tasks[i].state == TASK_RUNNING)
            count++;
    }
    return count;
}

bool task_get_info(uint32_t index, task_info_t *info)
{
    if (!info)
        return false;

    uint32_t active_index = 0;
    for (uint32_t i = 0; i < TASK_LIMIT; i++)
    {
        task_t *task = &tasks[i];
        if (task->state != TASK_READY &&
            task->state != TASK_RUNNING)
            continue;

        if (active_index++ != index)
            continue;

        info->pid = task->pid;
        info->parent_pid = task->parent_pid;

        const char *state = task_state_name(task->state);
        uint32_t character = 0;
        while (state[character] && character + 1 < sizeof(info->state))
        {
            info->state[character] = state[character];
            character++;
        }
        info->state[character] = '\0';
        return true;
    }

    return false;
}

bool task_user_range_valid(
    uint64_t address,
    uint64_t size,
    bool writable
)
{
    return current_task &&
           current_task != idle_task &&
           vmm_user_range_valid(
               current_task->address_space,
               address,
               size,
               writable
           );
}

bool task_copy_from_user(void *destination, uint64_t source, uint64_t size)
{
    if ((!destination && size) ||
        !task_user_range_valid(source, size, false))
        return false;

    bytes_copy(destination, (const void *)(uintptr_t)source, size);
    return true;
}

bool task_copy_to_user(uint64_t destination, const void *source, uint64_t size)
{
    if ((!source && size) ||
        !task_user_range_valid(destination, size, true))
        return false;

    bytes_copy((void *)(uintptr_t)destination, source, size);
    return true;
}

uint64_t task_allocate_user(uint64_t size)
{
    if (!current_task || current_task == idle_task ||
        size == 0 || size > UINT64_MAX - (VMM_PAGE_SIZE - 1))
        return 0;

    int64_t address = task_map_user(0, size, 0x3);
    return address < 0 ? 0 : (uint64_t)address;
}

static bool user_range_unmapped(uint64_t address, uint64_t pages)
{
    uint64_t end = address + pages * VMM_PAGE_SIZE;
    for (uint32_t i = 0; i < TASK_MAX_ALLOCS; i++)
    {
        const task_allocation_t *allocation =
            &current_task->allocations[i];
        if (!allocation->address)
            continue;
        uint64_t allocation_end =
            allocation->address + allocation->pages * VMM_PAGE_SIZE;
        if (address < allocation_end && allocation->address < end)
            return false;
    }
    return true;
}

static bool find_user_mapping_range(
    uint64_t start,
    uint64_t pages,
    uint64_t *address
)
{
    uint64_t bytes = pages * VMM_PAGE_SIZE;
    for (uint64_t candidate = start;
         candidate <= USER_ALLOC_LIMIT - bytes;)
    {
        uint64_t next_candidate = candidate;
        bool available = true;
        for (uint32_t i = 0; i < TASK_MAX_ALLOCS; i++)
        {
            const task_allocation_t *allocation =
                &current_task->allocations[i];
            if (!allocation->address)
                continue;
            uint64_t allocation_end =
                allocation->address +
                allocation->pages * VMM_PAGE_SIZE;
            if (candidate < allocation_end &&
                allocation->address < candidate + bytes)
            {
                available = false;
                if (allocation_end > next_candidate)
                    next_candidate = allocation_end;
            }
        }
        if (available)
        {
            *address = candidate;
            return true;
        }
        if (next_candidate <= candidate)
            return false;
        candidate = next_candidate;
    }
    return false;
}

int64_t task_map_user(
    uint64_t hint,
    uint64_t size,
    uint64_t protection
)
{
    if (!current_task || current_task == idle_task)
        return -SYSCALL_EINVAL;
    if (!size || size > UINT64_MAX - (VMM_PAGE_SIZE - 1) ||
        (protection & ~0x7ULL) || protection == 0)
        return -SYSCALL_EINVAL;

    uint64_t pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    uint64_t bytes = pages * VMM_PAGE_SIZE;
    if (bytes > USER_ALLOC_LIMIT - USER_ALLOC_BASE)
        return -SYSCALL_ENOMEM;

    uint32_t slot = TASK_MAX_ALLOCS;
    for (uint32_t i = 0; i < TASK_MAX_ALLOCS; i++)
    {
        if (!current_task->allocations[i].address)
        {
            slot = i;
            break;
        }
    }
    if (slot == TASK_MAX_ALLOCS)
        return -SYSCALL_ENOMEM;

    uint64_t address = 0;
    if (hint >= USER_ALLOC_BASE && hint < USER_ALLOC_LIMIT)
    {
        uint64_t aligned_hint =
            (hint + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);
        if (aligned_hint >= hint && aligned_hint <= USER_ALLOC_LIMIT - bytes &&
            user_range_unmapped(aligned_hint, pages))
            address = aligned_hint;
    }
    if (!address &&
        !find_user_mapping_range(USER_ALLOC_BASE, pages, &address))
        return -SYSCALL_ENOMEM;

    uint64_t mapped = 0;
    for (; mapped < pages; mapped++)
    {
        if (!allocate_user_page(
                current_task->address_space,
                address + mapped * VMM_PAGE_SIZE,
                protection))
            break;
    }
    if (mapped != pages)
    {
        while (mapped)
        {
            mapped--;
            uint64_t va = address + mapped * VMM_PAGE_SIZE;
            uint64_t pa = vmm_translate_in(
                current_task->address_space, va);
            if (vmm_unmap_page_in(current_task->address_space, va) && pa)
                pmm_free_page(pa);
        }
        return -SYSCALL_ENOMEM;
    }

    current_task->allocations[slot].address = address;
    current_task->allocations[slot].pages = pages;
    return (int64_t)address;
}

int64_t task_unmap_user(uint64_t address, uint64_t size)
{
    if (!current_task || current_task == idle_task || !address ||
        !size || address % VMM_PAGE_SIZE ||
        size > UINT64_MAX - (VMM_PAGE_SIZE - 1))
        return -SYSCALL_EINVAL;

    uint64_t pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    uint64_t bytes = pages * VMM_PAGE_SIZE;
    if (address > UINT64_MAX - bytes)
        return -SYSCALL_EINVAL;

    uint64_t end = address + bytes;
    uint32_t slot = TASK_MAX_ALLOCS;
    for (uint32_t i = 0; i < TASK_MAX_ALLOCS; i++)
    {
        const task_allocation_t *allocation =
            &current_task->allocations[i];
        uint64_t allocation_end = allocation->address +
            allocation->pages * VMM_PAGE_SIZE;
        if (allocation->address && address >= allocation->address &&
            end <= allocation_end)
        {
            slot = i;
            break;
        }
    }
    if (slot == TASK_MAX_ALLOCS)
        return -SYSCALL_EINVAL;

    task_allocation_t *allocation = &current_task->allocations[slot];
    uint64_t allocation_end =
        allocation->address + allocation->pages * VMM_PAGE_SIZE;
    bool split = address > allocation->address && end < allocation_end;
    uint32_t split_slot = TASK_MAX_ALLOCS;
    if (split)
    {
        for (uint32_t i = 0; i < TASK_MAX_ALLOCS; i++)
        {
            if (!current_task->allocations[i].address)
            {
                split_slot = i;
                break;
            }
        }
        if (split_slot == TASK_MAX_ALLOCS)
            return -SYSCALL_ENOMEM;
    }

    for (uint64_t page = 0; page < pages; page++)
    {
        uint64_t va = address + page * VMM_PAGE_SIZE;
        if (!vmm_translate_in(current_task->address_space, va))
            return -SYSCALL_EINVAL;
    }
    for (uint64_t page = 0; page < pages; page++)
    {
        uint64_t va = address + page * VMM_PAGE_SIZE;
        uint64_t pa = vmm_translate_in(current_task->address_space, va);
        if (!vmm_unmap_page_in(current_task->address_space, va) ||
            !pmm_free_page(pa))
            return -SYSCALL_EINVAL;
    }

    if (address == allocation->address && end == allocation_end)
    {
        *allocation = (task_allocation_t){0};
    }
    else if (address == allocation->address)
    {
        allocation->address = end;
        allocation->pages -= pages;
    }
    else if (end == allocation_end)
    {
        allocation->pages -= pages;
    }
    else
    {
        uint64_t trailing_pages = (allocation_end - end) / VMM_PAGE_SIZE;
        current_task->allocations[split_slot] = (task_allocation_t){
            .address = end,
            .pages = trailing_pages
        };
        allocation->pages =
            (address - allocation->address) / VMM_PAGE_SIZE;
    }
    return 0;
}

int64_t task_protect_user(
    uint64_t address,
    uint64_t size,
    uint64_t protection
)
{
    if (!current_task || current_task == idle_task || !address ||
        !size || address % VMM_PAGE_SIZE ||
        size > UINT64_MAX - (VMM_PAGE_SIZE - 1) ||
        (protection & ~0x7ULL))
        return -SYSCALL_EINVAL;

    uint64_t pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    uint64_t bytes = pages * VMM_PAGE_SIZE;
    if (address > UINT64_MAX - bytes)
        return -SYSCALL_EINVAL;
    uint64_t end = address + bytes;

    bool covered = false;
    for (uint32_t i = 0; i < TASK_MAX_ALLOCS; i++)
    {
        const task_allocation_t *allocation =
            &current_task->allocations[i];
        if (!allocation->address)
            continue;
        uint64_t allocation_end =
            allocation->address + allocation->pages * VMM_PAGE_SIZE;
        if (address >= allocation->address && end <= allocation_end)
        {
            covered = true;
            break;
        }
    }
    if (!covered)
        return -SYSCALL_EINVAL;

    for (uint64_t page = 0; page < pages; page++)
    {
        uint64_t virtual_address = address + page * VMM_PAGE_SIZE;
        if (!vmm_translate_in(
                current_task->address_space, virtual_address))
            return -SYSCALL_EINVAL;
    }

    uint64_t flags = 0;
    if (protection != 0)
        flags |= VMM_USER;
    if (protection & 0x2)
        flags |= VMM_WRITABLE;
    if (!(protection & 0x4) && paging_nx_supported())
        flags |= VMM_NO_EXECUTE;

    for (uint64_t page = 0; page < pages; page++)
    {
        uint64_t virtual_address = address + page * VMM_PAGE_SIZE;
        if (!vmm_protect_page_in(
                current_task->address_space,
                virtual_address,
                flags))
            return -SYSCALL_EINVAL;
    }
    return 0;
}

bool task_free_user(uint64_t address)
{
    if (!current_task || current_task == idle_task || !address)
        return false;

    for (uint32_t i = 0; i < TASK_MAX_ALLOCS; i++)
    {
        task_allocation_t *allocation =
            &current_task->allocations[i];

        if (allocation->address != address)
            continue;

        for (uint64_t page = 0; page < allocation->pages; page++)
        {
            uint64_t va = address + page * VMM_PAGE_SIZE;
            uint64_t pa = vmm_translate_in(
                current_task->address_space, va);

            if (!pa ||
                !vmm_unmap_page_in(current_task->address_space, va) ||
                !pmm_free_page(pa))
                return false;
        }

        allocation->address = 0;
        allocation->pages = 0;
        return true;
    }

    return false;
}

int64_t task_spawn_user(uint64_t entry)
{
    if (!current_task || current_task == idle_task ||
        !vmm_user_address_executable(
            current_task->address_space, entry))
        return -SYSCALL_EINVAL;

    task_t *task = allocate_task_slot();
    if (!task)
        return -SYSCALL_ENOMEM;

    task->address_space =
        vmm_clone_address_space(current_task->address_space);
    if (!task->address_space)
        goto fail;

    if (!allocate_kernel_stack(task) ||
        !map_user_stack(task->address_space, true))
        goto fail;

    task->pid = next_pid++;
    task->parent_pid = current_task->pid;
    task->state = TASK_READY;

    for (uint32_t i = 0; i < TASK_MAX_ALLOCS; i++)
        task->allocations[i] = current_task->allocations[i];
    for (uint32_t i = 0; i < TASK_MAX_FILES; i++)
    {
        if (current_task->files[i].used &&
            !current_task->files[i].is_socket)
        {
            task->files[i] = current_task->files[i];
            task->files[i].used = false;
            if (!vfs_clone_file(
                    &task->files[i].file,
                    &current_task->files[i].file))
                goto fail;
            task->files[i].used = true;
        }
    }

    task->frame = make_user_frame(task, entry, 0);
    return (int64_t)task->pid;

fail:
    for (uint32_t i = 0; i < TASK_MAX_FILES; i++)
    {
        if (task->files[i].used && !task->files[i].is_socket)
            vfs_close_file(&task->files[i].file);
        release_local_socket(&task->files[i]);
    }
    if (task->address_space)
        vmm_destroy_address_space(task->address_space);
    if (task->kernel_stack)
        kfree(task->kernel_stack);
    bytes_zero(task, sizeof(*task));
    return -SYSCALL_ENOMEM;
}

static int64_t task_spawn_elf_internal(
    const char *path,
    const char *argument,
    bool capture_output
);

int64_t task_spawn_elf(const char *path)
{
    return task_spawn_elf_internal(path, NULL, false);
}

int64_t task_spawn_elf_command(const char *path, const char *argument)
{
    return task_spawn_elf_internal(path, argument, true);
}

static int64_t task_spawn_elf_internal(
    const char *path,
    const char *argument,
    bool capture_output
)
{
    if (!current_task || current_task == idle_task || !path)
        return -SYSCALL_EINVAL;

    task_t *task = allocate_task_slot();
    if (!task)
        return -SYSCALL_ENOMEM;

    task->address_space = vmm_create_address_space();
    if (!task->address_space)
        goto fail;

    if (!allocate_kernel_stack(task))
        goto fail;

    uint64_t entry;
    int64_t result = elf_load_executable(
        task->address_space,
        path,
        &entry
    );
    if (result < 0)
    {
        if (task->address_space)
            vmm_destroy_address_space(task->address_space);
        if (task->kernel_stack)
            kfree(task->kernel_stack);
        bytes_zero(task, sizeof(*task));
        return result;
    }

    if (!map_user_stack(task->address_space, false))
        goto fail;

    uint64_t user_stack_pointer;
    if (!prepare_command_stack(
            task,
            path,
            argument,
            &user_stack_pointer))
        goto fail;

    task->pid = next_pid++;
    task->parent_pid = current_task->pid;
    task->state = TASK_READY;
    task->capture_output = capture_output;
    task->frame = make_user_frame(task, entry, 0);
    task->frame->user_rsp = user_stack_pointer;
    return (int64_t)task->pid;

fail:
    if (task->address_space)
        vmm_destroy_address_space(task->address_space);
    if (task->kernel_stack)
        kfree(task->kernel_stack);
    bytes_zero(task, sizeof(*task));
    return -SYSCALL_ENOMEM;
}

int64_t task_write_parent_output(const void *buffer, uint64_t size)
{
    if (!current_task || !current_task->capture_output ||
        !buffer || !size)
        return size ? -SYSCALL_EBADF : 0;
    task_t *parent = NULL;
    for (uint32_t i = 0; i < TASK_LIMIT; i++)
    {
        if (tasks[i].pid == current_task->parent_pid &&
            tasks[i].state != TASK_UNUSED &&
            tasks[i].state != TASK_ZOMBIE)
        {
            parent = &tasks[i];
            break;
        }
    }
    if (!parent)
        return -SYSCALL_EBADF;
    uint64_t written = 0;
    const char *source = buffer;
    while (written < size &&
           parent->child_output_used < TASK_CHILD_OUTPUT_SIZE)
    {
        parent->child_output[parent->child_output_write] =
            source[written++];
        parent->child_output_write =
            (parent->child_output_write + 1) % TASK_CHILD_OUTPUT_SIZE;
        parent->child_output_used++;
    }
    return written ? (int64_t)written : -SYSCALL_EAGAIN;
}

int64_t task_read_child_output(uint64_t buffer, uint64_t size)
{
    if (!current_task || current_task == idle_task)
        return -SYSCALL_EBADF;
    if (size > TASK_CHILD_OUTPUT_SIZE)
        size = TASK_CHILD_OUTPUT_SIZE;
    if (!task_user_range_valid(buffer, size, true))
        return -SYSCALL_EFAULT;
    uint64_t received = 0;
    while (received < size && current_task->child_output_used)
    {
        char character =
            current_task->child_output[current_task->child_output_read];
        if (!task_copy_to_user(buffer + received, &character, 1))
            return received ? (int64_t)received : -SYSCALL_EFAULT;
        current_task->child_output_read =
            (current_task->child_output_read + 1) % TASK_CHILD_OUTPUT_SIZE;
        current_task->child_output_used--;
        received++;
    }
    return (int64_t)received;
}

bool task_pid_is_active(uint64_t pid)
{
    if (pid == 0)
        return false;
    for (uint32_t i = 0; i < TASK_LIMIT; i++)
    {
        if (tasks[i].pid == pid &&
            (tasks[i].state == TASK_READY ||
             tasks[i].state == TASK_RUNNING))
            return true;
    }
    return false;
}

int64_t task_open_file(const char *path, int flags)
{
    if (!current_task || current_task == idle_task || !path)
        return -SYSCALL_EINVAL;
    const int access_mode = flags & 3;
    const int allowed_flags = 3 | 0x40 | 0x200 | 0x400;
    if (access_mode > 2 || (flags & ~allowed_flags) ||
        ((flags & 0x200) && access_mode == 0))
        return -SYSCALL_EINVAL;

    for (uint32_t i = 0; i < TASK_MAX_FILES; i++)
    {
        if (!current_task->files[i].used)
        {
            vfs_file_t file;
            int64_t result;
            if (access_mode == 0)
            {
                result = vfs_open(path, &file);
                if (result == -SYSCALL_ENOENT && (flags & 0x40))
                    result = vfs_open_write(path, true, false, &file);
            }
            else
                result = vfs_open_write(
                    path,
                    (flags & 0x40) != 0,
                    (flags & 0x200) != 0,
                    &file);
            if (result < 0)
                return result;

            current_task->files[i].used = true;
            current_task->files[i].readable = access_mode != 1;
            current_task->files[i].writable = access_mode != 0;
            current_task->files[i].append = (flags & 0x400) != 0;
            current_task->files[i].file = file;
            if (current_task->files[i].append)
                current_task->files[i].file.offset = file.size;
            return (int64_t)i + 3;
        }
    }

    return -SYSCALL_ENOMEM;
}

int64_t task_read_file(
    int64_t descriptor,
    uint64_t buffer,
    uint64_t size
)
{
    if (!current_task || current_task == idle_task ||
        descriptor < 3 ||
        descriptor >= TASK_MAX_FILES + 3)
        return -SYSCALL_EBADF;

    if (!task_user_range_valid(buffer, size, true))
        return -SYSCALL_EFAULT;

    task_file_t *file =
        &current_task->files[descriptor - 3];
    if (!file->used)
        return -SYSCALL_EBADF;
    if (!file->readable)
        return -SYSCALL_EBADF;
    if (size == 0)
        return 0;
    if (file->is_socket)
        return socket_read_file(file, buffer, size);

    uint8_t chunk[512];
    uint64_t total = 0;

    while (total < size)
    {
        uint32_t amount =
            (uint32_t)(size - total);
        if (amount > sizeof(chunk))
            amount = sizeof(chunk);

        int64_t received = vfs_read(
            &file->file,
            chunk,
            amount
        );
        if (received < 0)
            return total ? (int64_t)total : received;
        if (received == 0)
            break;

        if (!task_copy_to_user(
                buffer + total,
                chunk,
                (uint64_t)received))
            return total ? (int64_t)total : -SYSCALL_EFAULT;

        total += (uint64_t)received;
    }

    return (int64_t)total;
}

int64_t task_write_file(
    int64_t descriptor,
    const void *buffer,
    uint64_t size
)
{
    if (!current_task || current_task == idle_task ||
        descriptor < 3 ||
        descriptor >= TASK_MAX_FILES + 3)
        return -SYSCALL_EBADF;
    if (!buffer && size)
        return -SYSCALL_EFAULT;

    task_file_t *file = &current_task->files[descriptor - 3];
    if (!file->used)
        return -SYSCALL_EBADF;
    if (!file->writable)
        return -SYSCALL_EBADF;
    if (file->is_socket)
        return socket_write_file(file, buffer, size);

    if (file->append)
        file->file.offset = file->file.size;
    return vfs_write(&file->file, buffer, size);
}

int64_t task_seek_file(int64_t descriptor, int64_t offset, int whence)
{
    if (!current_task || current_task == idle_task ||
        descriptor < 3 ||
        descriptor >= TASK_MAX_FILES + 3)
        return -SYSCALL_EBADF;

    task_file_t *file = &current_task->files[descriptor - 3];
    if (!file->used)
        return -SYSCALL_EBADF;
    if (file->is_socket)
        return -SYSCALL_ESPIPE;
    if (file->file.size == UINT64_MAX)
        return -SYSCALL_ESPIPE;

    int64_t base;
    switch (whence)
    {
        case 0:
            base = 0;
            break;
        case 1:
            if (file->file.offset > INT64_MAX)
                return -SYSCALL_EINVAL;
            base = (int64_t)file->file.offset;
            break;
        case 2:
            if (file->file.size > INT64_MAX)
                return -SYSCALL_EINVAL;
            base = (int64_t)file->file.size;
            break;
        default:
            return -SYSCALL_EINVAL;
    }

    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset))
        return -SYSCALL_EINVAL;

    int64_t position = base + offset;
    if (position < 0)
        return -SYSCALL_EINVAL;

    file->file.offset = (uint64_t)position;
    return position;
}

int64_t task_close_file(int64_t descriptor)
{
    if (!current_task || current_task == idle_task ||
        descriptor < 3 ||
        descriptor >= TASK_MAX_FILES + 3)
        return -SYSCALL_EBADF;

    task_file_t *file =
        &current_task->files[descriptor - 3];
    if (!file->used)
        return -SYSCALL_EBADF;

    release_local_socket(file);
    if (!file->is_socket)
        vfs_close_file(&file->file);
    bytes_zero(file, sizeof(*file));
    return 0;
}

int64_t task_socketpair(int domain, int type, int protocol, int descriptors[2])
{
    if (!current_task || current_task == idle_task || !descriptors)
        return -SYSCALL_EINVAL;
    if (domain != AF_UNIX)
        return -SYSCALL_EAFNOSUPPORT;
    if (type != SOCK_STREAM)
        return -SYSCALL_EPROTONOSUPPORT;
    if (protocol != 0)
        return -SYSCALL_EPROTONOSUPPORT;

    uint32_t connection_id = TASK_MAX_LOCAL_CONNECTIONS;
    for (uint32_t i = 0; i < TASK_MAX_LOCAL_CONNECTIONS; i++)
    {
        if (!local_connections[i].used)
        {
            connection_id = i;
            break;
        }
    }
    if (connection_id == TASK_MAX_LOCAL_CONNECTIONS)
        return -SYSCALL_ENOMEM;

    uint32_t slots[2] = {TASK_MAX_FILES, TASK_MAX_FILES};
    for (uint32_t i = 0; i < TASK_MAX_FILES && slots[1] == TASK_MAX_FILES; i++)
    {
        if (!current_task->files[i].used)
        {
            if (slots[0] == TASK_MAX_FILES)
                slots[0] = i;
            else
                slots[1] = i;
        }
    }
    if (slots[1] == TASK_MAX_FILES)
        return -SYSCALL_ENOMEM;

    task_local_connection_t *connection =
        &local_connections[connection_id];
    bytes_zero(connection, sizeof(*connection));
    connection->used = true;
    connection->open_ends = 0x3;

    for (uint32_t side = 0; side < 2; side++)
    {
        task_file_t *file = &current_task->files[slots[side]];
        bytes_zero(file, sizeof(*file));
        file->used = true;
        file->is_socket = true;
        file->socket_connection = (uint8_t)connection_id;
        file->socket_side = (uint8_t)side;
        descriptors[side] = (int)slots[side] + 3;
    }
    return 0;
}

int64_t task_socket_readiness(
    int64_t descriptor,
    short events,
    short *revents
)
{
    if (!current_task || current_task == idle_task || !revents)
        return -SYSCALL_EINVAL;
    if (descriptor < 3 || descriptor >= TASK_MAX_FILES + 3)
        return -SYSCALL_EBADF;
    task_file_t *file = &current_task->files[descriptor - 3];
    if (!file->used)
        return -SYSCALL_EBADF;
    if (!file->is_socket)
    {
        *revents = (short)(events & (TASK_POLLIN | TASK_POLLOUT));
        return 0;
    }

    task_local_connection_t *connection =
        &local_connections[file->socket_connection];
    uint8_t incoming = (uint8_t)(1U - file->socket_side);
    uint8_t peer_mask = (uint8_t)(1U << incoming);
    short ready = 0;
    if ((events & TASK_POLLIN) &&
        (connection->used_bytes[incoming] ||
         !(connection->open_ends & peer_mask)))
        ready |= TASK_POLLIN;
    if (events & TASK_POLLOUT)
    {
        if (!(connection->open_ends & peer_mask))
            ready |= TASK_POLLERR;
        else if (connection->used_bytes[file->socket_side] <
                 TASK_SOCKET_BUFFER_SIZE)
            ready |= TASK_POLLOUT;
    }
    if (!(connection->open_ends & peer_mask))
        ready |= TASK_POLLHUP;
    *revents = ready;
    return 0;
}
