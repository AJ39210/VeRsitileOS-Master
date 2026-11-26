
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SERIAL_LOG_DEBUG,
    SERIAL_LOG_INFO,
    SERIAL_LOG_WARN,
    SERIAL_LOG_ERROR
} serial_log_level_t;

bool serial_init(void);

void serial_putc(char c);
void serial_write(const char *str);
void serial_console_write(const char *buffer, uint64_t size);
void serial_console_replay(void);
void serial_console_reset_cursor(void);
void serial_console_set_color(uint32_t color);
void serial_install_progress_begin(void);
void serial_install_progress_update(uint32_t percent);
void serial_install_progress_error(const char *message);
void serial_install_progress_complete(void);
void serial_systemd_log_control(bool enabled);
void serial_systemd_log_write(const char *buffer, uint64_t size);
uint64_t serial_systemd_log_size(void);
uint64_t serial_systemd_log_read(
    uint64_t offset,
    void *buffer,
    uint64_t size
);
uint64_t serial_boot_log_size(void);
uint64_t serial_boot_log_read(
    uint64_t offset,
    void *buffer,
    uint64_t size
);

void serial_write_dec(uint64_t value);
void serial_write_hex(uint64_t value);

void serial_log_begin(serial_log_level_t level);
void serial_log_end(void);
void serial_log(serial_log_level_t level, const char *message);

#endif
