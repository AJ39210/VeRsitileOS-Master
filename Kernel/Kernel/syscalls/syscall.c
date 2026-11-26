#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "idt.h"
#include "installer.h"
#include "drm.h"
#include "keyboard.h"
#include "module.h"
#include "mouse.h"
#include "panic.h"
#include "serial.h"
#include "syscall.h"
#include "task.h"
#include "timer.h"
#include "vesa.h"
#include "network.h"
#include "vfs.h"
#include "rtc.h"
#include "os/gui.h"
#include "poll.h"
#include "sys/socket.h"

#define SYSCALL_MAX_IO 65536
#define SYSCALL_PATH_MAX 256
#define SYSCALL_READDIR_MAX 8
#define SYSCALL_SCREEN_MAX_BYTES (64 * 1024 * 1024)
#define SYSCALL_DRAW_BATCH_MAX 128

extern void syscall_stub(void);

static os_draw_command_t syscall_draw_batch_commands[
    SYSCALL_DRAW_BATCH_MAX
];

static int64_t syscall_write(
    int64_t descriptor,
    uint64_t buffer,
    uint64_t size
)
{
    if (descriptor != 1 && descriptor != 2 && descriptor < 3)
        return -SYSCALL_EBADF;

    if (size > SYSCALL_MAX_IO)
        return -SYSCALL_EINVAL;

    if (!task_user_range_valid(buffer, size, false))
        return -SYSCALL_EFAULT;

    char chunk[128];
    uint64_t written = 0;

    while (written < size)
    {
        uint64_t amount = size - written;
        if (amount > sizeof(chunk))
            amount = sizeof(chunk);

        if (!task_copy_from_user(
                chunk,
                buffer + written,
                amount))
            return written ? (int64_t)written : -SYSCALL_EFAULT;

        int64_t result;
        if (descriptor == 1 || descriptor == 2)
        {
            result = task_write_parent_output(chunk, amount);
            if (result == -SYSCALL_EBADF)
            {
                if (task_current_pid() == 1)
                    serial_systemd_log_write(chunk, amount);
                serial_console_write(chunk, amount);
                result = (int64_t)amount;
            }
        }
        else
        {
            result = task_write_file(
                descriptor,
                chunk,
                amount
            );
        }

        if (result < 0)
            return written ? (int64_t)written : result;
        written += (uint64_t)result;
        if ((uint64_t)result < amount)
            break;

    }

    return (int64_t)written;
}

static int64_t syscall_read(
    int64_t descriptor,
    uint64_t buffer,
    uint64_t size
)
{
    if (size > SYSCALL_MAX_IO)
        return -SYSCALL_EINVAL;

    if (size == 0)
    {
        return descriptor == 0
            ? 0
            : task_read_file(descriptor, buffer, 0);
    }

    if (!task_user_range_valid(buffer, size, true))
        return -SYSCALL_EFAULT;

    if (descriptor == 0)
    {
        uint64_t received = 0;
        while (received < size && keyboard_has_input())
        {
            char character = keyboard_getchar();
            if (!task_copy_to_user(
                    buffer + received,
                    &character,
                    sizeof(character)))
            {
                return received
                    ? (int64_t)received
                    : -SYSCALL_EFAULT;
            }
            received++;
        }

        return received ? (int64_t)received : -SYSCALL_EAGAIN;
    }

    return task_read_file(descriptor, buffer, size);
}

static int64_t syscall_open(uint64_t user_path, uint64_t flags)
{
    if (flags > 0x7FFFFFFFULL)
        return -SYSCALL_EINVAL;
    char path[SYSCALL_PATH_MAX];

    for (uint64_t i = 0; i < sizeof(path); i++)
    {
        if (!task_copy_from_user(
                &path[i],
                user_path + i,
                sizeof(path[i])))
            return -SYSCALL_EFAULT;

        if (path[i] == '\0')
            return task_open_file(path, (int)flags);
    }

    return -SYSCALL_ENAMETOOLONG;
}

static int64_t syscall_copy_path(uint64_t user_path, char path[SYSCALL_PATH_MAX])
{
    for (uint64_t i = 0; i < SYSCALL_PATH_MAX; i++)
    {
        if (!task_copy_from_user(
                &path[i],
                user_path + i,
                sizeof(path[i])))
            return -SYSCALL_EFAULT;
        if (path[i] == '\0')
            return 0;
    }
    return -SYSCALL_ENAMETOOLONG;
}

static int64_t syscall_execute_draw(const os_draw_command_t *command)
{
    if (command->type == OS_DRAW_RECTANGLE)
    {
        if (!drm_fill_rect(
            command->x,
            command->y,
            command->width,
            command->height,
            command->color
        ))
            return -SYSCALL_ENODEV;
        return 0;
    }
    if (command->type == OS_DRAW_TEXT)
    {
        if (command->text_length > sizeof(command->text))
            return -SYSCALL_EINVAL;
        if (!drm_draw_text(
            command->x,
            command->y,
            command->color,
            command->text,
            command->text_length
        ))
            return -SYSCALL_ENODEV;
        return 0;
    }
    return -SYSCALL_EINVAL;
}

static int64_t syscall_draw(uint64_t user_command)
{
    os_draw_command_t command;
    if (!task_copy_from_user(
            &command,
            user_command,
            sizeof(command)))
        return -SYSCALL_EFAULT;
    return syscall_execute_draw(&command);
}

static int64_t syscall_draw_batch(uint64_t user_commands, uint64_t count)
{
    if (!count || count > SYSCALL_DRAW_BATCH_MAX)
        return -SYSCALL_EINVAL;

    uint64_t bytes = count * sizeof(syscall_draw_batch_commands[0]);
    if (!task_user_range_valid(user_commands, bytes, false) ||
        !task_copy_from_user(
            syscall_draw_batch_commands,
            user_commands,
            bytes))
        return -SYSCALL_EFAULT;

    for (uint64_t i = 0; i < count; i++)
    {
        const os_draw_command_t *command =
            &syscall_draw_batch_commands[i];
        if ((command->type != OS_DRAW_RECTANGLE &&
             command->type != OS_DRAW_TEXT) ||
            (command->type == OS_DRAW_TEXT &&
             command->text_length > sizeof(command->text)))
            return -SYSCALL_EINVAL;
    }

    vesa_draw_batch_begin();
    int64_t result = 0;
    for (uint64_t i = 0; i < count; i++)
    {
        result = syscall_execute_draw(&syscall_draw_batch_commands[i]);
        if (result < 0)
            break;
    }
    vesa_draw_batch_end();
    return result;
}

static int64_t syscall_screen_region(uint64_t user_region, bool read_screen)
{
    os_screen_region_t region;
    if (!task_copy_from_user(&region, user_region, sizeof(region)))
        return -SYSCALL_EFAULT;
    if (region.width == 0 || region.height == 0 ||
        region.stride < region.width ||
        region.width > UINT64_MAX / sizeof(uint32_t) ||
        (region.height - 1) >
            (UINT64_MAX / sizeof(uint32_t) - region.width) /
                region.stride)
        return -SYSCALL_EINVAL;

    uint64_t bytes = (
        (region.height - 1) * region.stride + region.width
    ) * sizeof(uint32_t);
    if (bytes > SYSCALL_SCREEN_MAX_BYTES)
        return -SYSCALL_EINVAL;

    if (!task_user_range_valid(
            (uint64_t)(uintptr_t)region.pixels,
            bytes,
            read_screen))
        return -SYSCALL_EFAULT;

    bool success = read_screen
        ? drm_read_pixels(
            region.x,
            region.y,
            region.width,
            region.height,
            (uint32_t *)(uintptr_t)region.pixels,
            region.stride)
        : drm_blit(
            region.x,
            region.y,
            region.width,
            region.height,
            region.pixels,
            region.stride);
    if (!success)
        return -SYSCALL_EINVAL;
    return 0;
}

static int64_t syscall_mouse_read(uint64_t user_buffer, uint64_t size)
{
    if (!mouse_is_initialized())
        return -SYSCALL_ENODEV;
    if (size < sizeof(mouse_event_t) ||
        size > 16 * sizeof(mouse_event_t) ||
        size % sizeof(mouse_event_t) != 0)
        return -SYSCALL_EINVAL;
    if (!task_user_range_valid(user_buffer, size, true))
        return -SYSCALL_EFAULT;

    mouse_event_t events[16];
    int64_t result = mouse_read_events(events, size);
    if (result < 0)
        return result;
    if (!task_copy_to_user(user_buffer, events, (uint64_t)result))
        return -SYSCALL_EFAULT;
    return result;
}

static int64_t syscall_keyboard_read(uint64_t user_buffer, uint64_t size)
{
    if (size < sizeof(keyboard_event_t) ||
        size > 16 * sizeof(keyboard_event_t) ||
        size % sizeof(keyboard_event_t) != 0)
        return -SYSCALL_EINVAL;
    if (!task_user_range_valid(user_buffer, size, true))
        return -SYSCALL_EFAULT;

    keyboard_poll();
    keyboard_event_t events[16];
    int64_t result = keyboard_read_events(events, size);
    if (result < 0)
        return -SYSCALL_EINVAL;
    if (result == 0)
        return -SYSCALL_EAGAIN;
    if (!task_copy_to_user(user_buffer, events, (uint64_t)result))
        return -SYSCALL_EFAULT;
    return result;
}

static int64_t syscall_socketpair(
    int domain,
    int type,
    int protocol,
    uint64_t user_descriptors
)
{
    int descriptors[2];
    int64_t result = task_socketpair(domain, type, protocol, descriptors);
    if (result < 0)
        return result;
    if (!task_copy_to_user(
            user_descriptors,
            descriptors,
            sizeof(descriptors)))
    {
        task_close_file(descriptors[0]);
        task_close_file(descriptors[1]);
        return -SYSCALL_EFAULT;
    }
    return 0;
}

static int64_t syscall_poll(uint64_t user_fds, uint64_t count)
{
    if (count > 16)
        return -SYSCALL_EINVAL;
    if (count && !task_user_range_valid(
            user_fds,
            count * sizeof(struct pollfd),
            true))
        return -SYSCALL_EFAULT;

    struct pollfd fds[16];
    if (count && !task_copy_from_user(
            fds,
            user_fds,
            count * sizeof(struct pollfd)))
        return -SYSCALL_EFAULT;

    int64_t ready_count = 0;
    for (uint64_t i = 0; i < count; i++)
    {
        fds[i].revents = 0;
        short revents = 0;
        int64_t result = task_socket_readiness(
            fds[i].fd,
            fds[i].events,
            &revents);
        if (result < 0)
        {
            if (result == -SYSCALL_EBADF)
                fds[i].revents = POLLNVAL;
            else
                return result;
        }
        else
        {
            fds[i].revents = revents;
        }
        if (fds[i].revents)
            ready_count++;
    }

    if (count && !task_copy_to_user(
            user_fds,
            fds,
            count * sizeof(struct pollfd)))
        return -SYSCALL_EFAULT;
    return ready_count;
}

static int64_t syscall_readdir(uint64_t user_request, uint64_t user_entries)
{
    os_directory_request_t request;
    if (!task_copy_from_user(
            &request,
            user_request,
            sizeof(request)))
        return -SYSCALL_EFAULT;
    if (request.capacity == 0 ||
        request.capacity > SYSCALL_READDIR_MAX)
        return -SYSCALL_EINVAL;

    vfs_dirent_t entries[SYSCALL_READDIR_MAX];
    int64_t count = vfs_list(
        request.path,
        request.offset,
        entries,
        request.capacity
    );
    if (count < 0)
        return count;

    uint64_t bytes = (uint64_t)count * sizeof(os_directory_entry_t);
    if (!task_user_range_valid(user_entries, bytes, true))
        return -SYSCALL_EFAULT;

    for (int64_t i = 0; i < count; i++)
    {
        os_directory_entry_t output = {0};
        for (uint32_t j = 0; j < sizeof(output.name); j++)
        {
            output.name[j] = entries[i].name[j];
            if (entries[i].name[j] == '\0')
                break;
        }
        output.size = entries[i].size;
        output.is_directory = entries[i].is_directory;
        if (!task_copy_to_user(
                user_entries + (uint64_t)i * sizeof(output),
                &output,
                sizeof(output)))
            return -SYSCALL_EFAULT;
    }
    return count;
}

irq_frame_t *syscall_dispatch(irq_frame_t *frame)
{
    if (!frame || (frame->cs & 3) != 3)
        return frame;

    int64_t result;
    bool terminate = false;
    bool schedule_now = false;

    switch (frame->rax)
    {
        case SYS_WRITE:
            result = syscall_write(
                (int64_t)frame->rdi,
                frame->rsi,
                frame->rdx
            );
            break;

        case SYS_READ:
            result = syscall_read(
                (int64_t)frame->rdi,
                frame->rsi,
                frame->rdx
            );
            break;

        case SYS_TIME_MS:
            result = (int64_t)timer_get_milliseconds();
            break;

        case SYS_RTC_READ:
        {
            if (!task_user_range_valid(
                    frame->rdi, sizeof(os_rtc_time_t), true))
            {
                result = -SYSCALL_EFAULT;
                break;
            }
            os_rtc_time_t time;
            if (!rtc_read_time(&time))
            {
                result = -SYSCALL_EIO;
                break;
            }
            result = task_copy_to_user(
                frame->rdi, &time, sizeof(time))
                ? 0 : -SYSCALL_EFAULT;
            break;
        }

        case SYS_ALLOC:
            result = (int64_t)task_allocate_user(frame->rdi);
            if (!result)
                result = -SYSCALL_ENOMEM;
            break;

        case SYS_FREE:
            result = task_free_user(frame->rdi)
                ? 0
                : -SYSCALL_EINVAL;
            break;

        case SYS_MMAP:
        {
            uint64_t protection = frame->rdx;
            uint64_t flags = frame->r10;
            int64_t descriptor = (int64_t)frame->r8;
            uint64_t offset = frame->r9;
            const uint64_t anonymous_private =
                0x02ULL | 0x20ULL;

            if (flags & 0x01ULL)
                result = -SYSCALL_ENOTSUP;
            else if (flags != anonymous_private ||
                     descriptor != -1 || offset != 0 ||
                     (protection & ~0x7ULL))
                result = -SYSCALL_EINVAL;
            else if (protection == 0)
                result = -SYSCALL_ENOTSUP;
            else
                result = task_map_user(
                    frame->rdi,
                    frame->rsi,
                    protection);
            break;
        }

        case SYS_MUNMAP:
            result = task_unmap_user(frame->rdi, frame->rsi);
            break;

        case SYS_MPROTECT:
            result = (frame->rdx & ~0x7ULL)
                ? -SYSCALL_EINVAL
                : task_protect_user(
                    frame->rdi,
                    frame->rsi,
                    frame->rdx);
            break;

        case SYS_SOCKETPAIR:
            result = syscall_socketpair(
                (int)frame->rdi,
                (int)frame->rsi,
                (int)frame->rdx,
                frame->r10);
            break;

        case SYS_POLL:
            result = syscall_poll(frame->rdi, frame->rsi);
            break;

        case SYS_OPEN:
            result = syscall_open(frame->rdi, frame->rsi);
            break;

        case SYS_CLOSE:
            result = task_close_file((int64_t)frame->rdi);
            break;

        case SYS_SEEK:
            result = task_seek_file(
                (int64_t)frame->rdi,
                (int64_t)frame->rsi,
                (int)frame->rdx
            );
            break;

        case SYS_SCREEN_INFO:
        {
            drm_device_info_t device_info;
            os_screen_info_t info;
            result = 0;
            if (!drm_get_device_info(&device_info))
                result = -SYSCALL_ENODEV;
            else
            {
                info.width = device_info.mode.width;
                info.height = device_info.mode.height;
            }
            if (result == 0 && !task_copy_to_user(
                         frame->rdi,
                         &info,
                         sizeof(info)))
                result = -SYSCALL_EFAULT;
            else if (result == 0)
                result = 0;
            break;
        }

        case SYS_DRAW:
            result = syscall_draw(frame->rdi);
            break;

        case SYS_DRAW_BATCH:
            result = syscall_draw_batch(frame->rdi, frame->rsi);
            break;

        case SYS_SCREEN_BLIT:
            result = syscall_screen_region(frame->rdi, false);
            break;

        case SYS_SCREEN_READ:
            result = syscall_screen_region(frame->rdi, true);
            break;

        case SYS_CONSOLE_CLEAR:
            vesa_clear(0x000000);
            serial_console_reset_cursor();
            result = 0;
            break;

        case SYS_CONSOLE_SET_COLOR:
            serial_console_set_color((uint32_t)frame->rdi);
            result = 0;
            break;

        case SYS_SYSTEMD_LOG_CONTROL:
            if (task_current_pid() != 1 || frame->rdi > 1)
                result = -SYSCALL_EPERM;
            else
            {
                serial_systemd_log_control(frame->rdi != 0);
                result = 0;
            }
            break;

        case SYS_SYSTEMD_WATCHDOG:
            if (task_current_pid() != 1 || frame->rdi > 3600)
                result = -SYSCALL_EPERM;
            else
            {
                timer_systemd_watchdog_feed((uint32_t)frame->rdi);
                result = 0;
            }
            break;

        case SYS_PANIC:
        {
            char message[SYSCALL_PATH_MAX];
            result = syscall_copy_path(frame->rdi, message);
            if (result == 0)
                kernel_panic(message);
            break;
        }

        case SYS_NETWORK_INFO:
        {
            if (!task_user_range_valid(
                    frame->rdi,
                    sizeof(network_info_t),
                    true))
            {
                result = -SYSCALL_EFAULT;
                break;
            }
            network_info_t info;
            network_get_info(&info);
            result = task_copy_to_user(
                frame->rdi,
                &info,
                sizeof(info))
                ? 0
                : -SYSCALL_EFAULT;
            break;
        }

        case SYS_NETWORK_PING:
        {
            network_ping_request_t request;
            if (!task_copy_from_user(
                    &request,
                    frame->rdi,
                    sizeof(request)))
            {
                result = -SYSCALL_EFAULT;
                break;
            }
            uint64_t round_trip_ms = 0;
            if (!network_ping(request.address, &round_trip_ms))
            {
                result = -SYSCALL_ETIMEDOUT;
                break;
            }
            request.round_trip_ms = round_trip_ms;
            result = task_copy_to_user(
                frame->rdi,
                &request,
                sizeof(request))
                ? 0
                : -SYSCALL_EFAULT;
            break;
        }

        case SYS_NETWORK_RESOLVE:
        {
            network_resolve_request_t request;
            if (!task_copy_from_user(
                    &request,
                    frame->rdi,
                    sizeof(request)))
            {
                result = -SYSCALL_EFAULT;
                break;
            }
            bool terminated = false;
            for (size_t i = 0; i < sizeof(request.hostname); i++)
            {
                if (!request.hostname[i])
                {
                    terminated = true;
                    break;
                }
            }
            if (!request.hostname[0] || !terminated)
            {
                result = -SYSCALL_EINVAL;
                break;
            }
            if (!network_resolve(request.hostname, request.address))
            {
                result = -SYSCALL_ETIMEDOUT;
                break;
            }
            result = task_copy_to_user(
                frame->rdi,
                &request,
                sizeof(request))
                ? 0
                : -SYSCALL_EFAULT;
            break;
        }

        case SYS_SPAWN_ELF_COMMAND:
        {
            os_spawn_command_t request;
            if (!task_copy_from_user(
                    &request,
                    frame->rdi,
                    sizeof(request)))
            {
                result = -SYSCALL_EFAULT;
                break;
            }
            if (!request.path[0] ||
                request.path[sizeof(request.path) - 1] != '\0' ||
                request.argument[sizeof(request.argument) - 1] != '\0')
            {
                result = -SYSCALL_EINVAL;
                break;
            }
            result = task_spawn_elf_command(
                request.path,
                request.argument[0] ? request.argument : NULL);
            if (result < 0)
            {
                serial_write("Command ELF spawn failed: ");
                serial_write(request.path);
                serial_write(" error=");
                serial_write_hex((uint64_t)-result);
                serial_putc('\n');
            }
            break;
        }

        case SYS_READ_CHILD_OUTPUT:
            result = task_read_child_output(frame->rdi, frame->rsi);
            break;

        case SYS_MOUSE_READ:
            result = syscall_mouse_read(frame->rdi, frame->rsi);
            break;

        case SYS_KEYBOARD_READ:
            result = syscall_keyboard_read(frame->rdi, frame->rsi);
            break;

        case SYS_READDIR:
            result = syscall_readdir(frame->rdi, frame->rsi);
            break;

        case SYS_INSTALL:
        {
            os_install_request_t request;
            if (!task_copy_from_user(
                    &request,
                    frame->rdi,
                    sizeof(request)))
                result = -SYSCALL_EFAULT;
            else
                result = installer_run(&request);
            break;
        }

        case SYS_MODULE_LOAD:
        {
            char path[SYSCALL_PATH_MAX];
            result = syscall_copy_path(frame->rdi, path);
            if (result == 0)
                result = kernel_module_load(path);
            break;
        }

        case SYS_NETWORK_INIT:
            result = task_current_pid() == 1
                ? (network_init() ? 0 : -SYSCALL_ENODEV)
                : -SYSCALL_EPERM;
            break;

        case SYS_MKDIR:
        case SYS_RMDIR:
        case SYS_UNLINK:
        case SYS_ISDIR:
        {
            char path[SYSCALL_PATH_MAX];
            result = syscall_copy_path(frame->rdi, path);
            if (result == 0)
            {
                if (frame->rax == SYS_MKDIR)
                    result = vfs_mkdir(path);
                else if (frame->rax == SYS_RMDIR)
                    result = vfs_rmdir(path);
                else if (frame->rax == SYS_UNLINK)
                    result = vfs_unlink(path);
                else
                    result = vfs_is_directory(path)
                        ? 1
                        : -SYSCALL_ENOENT;
            }
            break;
        }

        case SYS_TASK_ACTIVE:
            result = task_pid_is_active(frame->rdi) ? 1 : 0;
            break;

        case SYS_SPAWN:
            result = task_spawn_user(frame->rdi);
            break;

        case SYS_SPAWN_ELF:
        {
            char path[SYSCALL_PATH_MAX];
            uint64_t i;
            bool path_copied = true;
            bool path_terminated = false;
            for (i = 0; i < sizeof(path); i++)
            {
                if (!task_copy_from_user(
                        &path[i],
                        frame->rdi + i,
                        sizeof(path[i])))
                {
                    path_copied = false;
                    break;
                }
                if (path[i] == '\0')
                {
                    path_terminated = true;
                    break;
                }
            }

            if (i == sizeof(path))
                result = -SYSCALL_ENAMETOOLONG;
            else if (!path_copied)
                result = -SYSCALL_EFAULT;
            else if (path_terminated)
            {
                result = task_spawn_elf(path);
                if (result < 0)
                {
                    serial_write("ELF spawn failed: ");
                    serial_write(path);
                    serial_write(" error=");
                    serial_write_hex((uint64_t)-result);
                    serial_putc('\n');
                }
            }
            break;
        }

        case SYS_EXIT:
            result = 0;
            terminate = true;
            schedule_now = true;
            break;

        case SYS_GETPID:
            result = (int64_t)task_current_pid();
            break;

        case SYS_YIELD:
            network_poll();
            result = 0;
            schedule_now = true;
            break;

        default:
            result = -SYSCALL_ENOSYS;
            break;
    }

    frame->rax = (uint64_t)result;

    if (schedule_now)
        return task_schedule_from_syscall(frame, terminate);

    return frame;
}

void syscall_init(void)
{
    idt_set_user_gate(SYSCALL_VECTOR, syscall_stub);
}
