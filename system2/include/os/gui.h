#ifndef VERSITILE_OS_GUI_H
#define VERSITILE_OS_GUI_H

#include <stdint.h>
#include <stddef.h>

typedef struct
{
    uint64_t width;
    uint64_t height;
} os_screen_info_t;

typedef struct
{
    uint32_t type;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t color;
    uint32_t text_length;
    char text[192];
} os_draw_command_t;

typedef struct
{
    int16_t dx;
    int16_t dy;
    uint8_t buttons;
    uint8_t reserved[3];
} os_mouse_event_t;

typedef struct
{
    uint8_t scancode;
    uint8_t character;
    uint8_t pressed;
    uint8_t shift;
    uint8_t caps_lock;
    uint8_t extended;
} os_keyboard_event_t;

typedef struct
{
    char name[256];
    uint64_t size;
    uint8_t is_directory;
    uint8_t reserved[7];
} os_directory_entry_t;

typedef struct
{
    char path[256];
    uint64_t offset;
    uint64_t capacity;
} os_directory_request_t;

typedef struct
{
    uint64_t x;
    uint64_t y;
    uint64_t width;
    uint64_t height;
    uint64_t stride;
    const uint32_t *pixels;
} os_screen_region_t;

typedef struct
{
    uint8_t mac[6];
    uint8_t address[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns[2][4];
    uint8_t link_up;
    uint8_t configured;
    uint8_t reserved[2];
} os_network_info_t;

typedef struct
{
    uint8_t address[4];
    uint8_t reserved[4];
    uint64_t round_trip_ms;
} os_network_ping_t;

typedef struct
{
    char hostname[256];
    uint8_t address[4];
} os_network_resolve_t;

typedef struct
{
    char path[256];
    char argument[256];
} os_spawn_command_t;

typedef struct
{
    char device[16];
    char username[32];
    char hostname[64];
    char confirmation[64];
} os_install_request_t;

typedef struct
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t reserved;
} os_rtc_time_t;

enum
{
    OS_DRAW_RECTANGLE = 1,
    OS_DRAW_TEXT = 2,
    OS_SYS_READ = 2,
    OS_SYS_SCREEN_INFO = 14,
    OS_SYS_DRAW = 15,
    OS_SYS_MOUSE_READ = 16,
    OS_SYS_READDIR = 17,
    OS_SYS_MKDIR = 18,
    OS_SYS_RMDIR = 19,
    OS_SYS_ISDIR = 20,
    OS_SYS_TASK_ACTIVE = 21,
    OS_SYS_SPAWN_ELF = 12,
    OS_SYS_SCREEN_BLIT = 22,
    OS_SYS_SCREEN_READ = 23,
    OS_SYS_PANIC = 24,
    OS_SYS_NETWORK_INFO = 25,
    OS_SYS_NETWORK_PING = 26,
    OS_SYS_SPAWN_ELF_COMMAND = 27,
    OS_SYS_READ_CHILD_OUTPUT = 28,
    OS_SYS_KEYBOARD_READ = 34,
    OS_SYS_NETWORK_RESOLVE = 35,
    OS_SYS_CONSOLE_CLEAR = 36,
    OS_SYS_CONSOLE_SET_COLOR = 37,
    OS_SYS_SYSTEMD_LOG_CONTROL = 38,
    OS_SYS_SYSTEMD_WATCHDOG = 39,
    OS_SYS_DRAW_BATCH = 40,
    OS_SYS_INSTALL = 41,
    OS_SYS_MODULE_LOAD = 42,
    OS_SYS_NETWORK_INIT = 43,
    OS_SYS_RTC_READ = 44,
    OS_SYS_TIME_MS = 3
};

static inline int64_t os_syscall0(uint64_t number)
{
    register uint64_t result __asm__("rax") = number;
    __asm__ volatile (
        "int $0x80"
        : "+a"(result)
        :
        : "memory", "cc"
    );
    return (int64_t)result;
}

static inline int64_t os_syscall1(uint64_t number, uint64_t first_arg)
{
    register uint64_t result __asm__("rax") = number;
    register uint64_t first __asm__("rdi") = first_arg;
    __asm__ volatile (
        "int $0x80"
        : "+a"(result), "+D"(first)
        :
        : "memory", "cc"
    );
    return (int64_t)result;
}

static inline int64_t os_syscall2(
    uint64_t number,
    uint64_t first_arg,
    uint64_t second_arg
)
{
    register uint64_t result __asm__("rax") = number;
    register uint64_t first __asm__("rdi") = first_arg;
    register uint64_t second __asm__("rsi") = second_arg;
    __asm__ volatile (
        "int $0x80"
        : "+a"(result), "+D"(first), "+S"(second)
        :
        : "memory", "cc"
    );
    return (int64_t)result;
}

static inline int64_t os_syscall3(
    uint64_t number,
    uint64_t first_arg,
    uint64_t second_arg,
    uint64_t third_arg
)
{
    register uint64_t result __asm__("rax") = number;
    register uint64_t first __asm__("rdi") = first_arg;
    register uint64_t second __asm__("rsi") = second_arg;
    register uint64_t third __asm__("rdx") = third_arg;
    __asm__ volatile (
        "int $0x80"
        : "+a"(result), "+D"(first), "+S"(second), "+d"(third)
        :
        : "memory", "cc"
    );
    return (int64_t)result;
}

static inline int64_t os_time_ms(void)
{
    return os_syscall0(OS_SYS_TIME_MS);
}

static inline int64_t os_rtc_read(os_rtc_time_t *time)
{
    return os_syscall1(
        OS_SYS_RTC_READ,
        (uint64_t)(uintptr_t)time);
}

static inline int64_t os_read(
    int descriptor,
    void *buffer,
    uint64_t size
)
{
    return os_syscall3(
        OS_SYS_READ,
        (uint64_t)descriptor,
        (uint64_t)(uintptr_t)buffer,
        size
    );
}

static inline int64_t os_screen_info(os_screen_info_t *info)
{
    return os_syscall1(OS_SYS_SCREEN_INFO, (uint64_t)(uintptr_t)info);
}

static inline int64_t os_console_clear(void)
{
    return os_syscall0(OS_SYS_CONSOLE_CLEAR);
}

static inline int64_t os_console_set_color(uint32_t color)
{
    return os_syscall1(OS_SYS_CONSOLE_SET_COLOR, color);
}

static inline int64_t os_systemd_log_control(int enabled)
{
    return os_syscall1(OS_SYS_SYSTEMD_LOG_CONTROL, enabled != 0);
}

static inline int64_t os_systemd_watchdog(uint32_t timeout_seconds)
{
    return os_syscall1(OS_SYS_SYSTEMD_WATCHDOG, timeout_seconds);
}

static inline int64_t os_draw(const os_draw_command_t *command)
{
    return os_syscall1(OS_SYS_DRAW, (uint64_t)(uintptr_t)command);
}

static inline int64_t os_draw_batch(
    const os_draw_command_t *commands,
    uint64_t count)
{
    return os_syscall2(
        OS_SYS_DRAW_BATCH,
        (uint64_t)(uintptr_t)commands,
        count
    );
}

static inline int64_t os_screen_blit(const os_screen_region_t *region)
{
    return os_syscall1(
        OS_SYS_SCREEN_BLIT,
        (uint64_t)(uintptr_t)region
    );
}

static inline int64_t os_screen_read(os_screen_region_t *region)
{
    return os_syscall1(
        OS_SYS_SCREEN_READ,
        (uint64_t)(uintptr_t)region
    );
}

static inline int64_t os_kernel_panic(const char *message)
{
    return os_syscall1(OS_SYS_PANIC, (uint64_t)(uintptr_t)message);
}

static inline int64_t os_network_info(os_network_info_t *info)
{
    return os_syscall1(
        OS_SYS_NETWORK_INFO,
        (uint64_t)(uintptr_t)info);
}

static inline int64_t os_network_ping(os_network_ping_t *request)
{
    return os_syscall1(
        OS_SYS_NETWORK_PING,
        (uint64_t)(uintptr_t)request);
}

static inline int64_t os_network_resolve(os_network_resolve_t *request)
{
    return os_syscall1(
        OS_SYS_NETWORK_RESOLVE,
        (uint64_t)(uintptr_t)request);
}

static inline int64_t os_spawn_elf_command(
    const char *path,
    const char *argument
)
{
    os_spawn_command_t request = {0};
    size_t path_length = 0;
    while (path && path[path_length] &&
           path_length < sizeof(request.path) - 1)
    {
        request.path[path_length] = path[path_length];
        path_length++;
    }
    if (!path || path[path_length])
        return -1;
    size_t argument_length = 0;
    while (argument && argument[argument_length] &&
           argument_length < sizeof(request.argument) - 1)
    {
        request.argument[argument_length] = argument[argument_length];
        argument_length++;
    }
    if (argument && argument[argument_length])
        return -1;
    return os_syscall1(
        OS_SYS_SPAWN_ELF_COMMAND,
        (uint64_t)(uintptr_t)&request);
}

static inline int64_t os_read_child_output(void *buffer, uint64_t size)
{
    return os_syscall2(
        OS_SYS_READ_CHILD_OUTPUT,
        (uint64_t)(uintptr_t)buffer,
        size);
}

static inline int64_t os_mouse_read(os_mouse_event_t *events, uint64_t size)
{
    return os_syscall2(
        OS_SYS_MOUSE_READ,
        (uint64_t)(uintptr_t)events,
        size
    );
}

static inline int64_t os_keyboard_read(
    os_keyboard_event_t *events,
    uint64_t size
)
{
    return os_syscall2(
        OS_SYS_KEYBOARD_READ,
        (uint64_t)(uintptr_t)events,
        size
    );
}

static inline int64_t os_readdir(
    os_directory_request_t *request,
    os_directory_entry_t *entries
)
{
    return os_syscall2(
        OS_SYS_READDIR,
        (uint64_t)(uintptr_t)request,
        (uint64_t)(uintptr_t)entries
    );
}

static inline int64_t os_mkdir(const char *path)
{
    return os_syscall1(OS_SYS_MKDIR, (uint64_t)(uintptr_t)path);
}

static inline int64_t os_rmdir(const char *path)
{
    return os_syscall1(OS_SYS_RMDIR, (uint64_t)(uintptr_t)path);
}

static inline int64_t os_isdir(const char *path)
{
    return os_syscall1(OS_SYS_ISDIR, (uint64_t)(uintptr_t)path);
}

static inline int64_t os_task_active(uint64_t pid)
{
    return os_syscall1(OS_SYS_TASK_ACTIVE, pid);
}

static inline int64_t os_spawn_elf(const char *path)
{
    return os_syscall1(
        OS_SYS_SPAWN_ELF,
        (uint64_t)(uintptr_t)path
    );
}

static inline int64_t os_install(const os_install_request_t *request)
{
    return os_syscall1(
        OS_SYS_INSTALL,
        (uint64_t)(uintptr_t)request);
}

static inline int64_t os_module_load(const char *path)
{
    return os_syscall1(
        OS_SYS_MODULE_LOAD,
        (uint64_t)(uintptr_t)path);
}

static inline int64_t os_network_init(void)
{
    return os_syscall0(OS_SYS_NETWORK_INIT);
}

#endif
