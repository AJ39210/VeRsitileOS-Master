
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "serial.h"
#include "timer.h"
#include "vesa.h"

#define COM1 0x3F8
#define CONSOLE_HISTORY_SIZE (64 * 1024)
#define SYSTEMD_LOG_SIZE (32 * 1024)
#define CONSOLE_COLOR_DEFAULT 0x00E6EDF3
#define CONSOLE_COLOR_DEBUG 0x0066D9EF
#define CONSOLE_COLOR_INFO 0x00FFFF00
#define CONSOLE_COLOR_WARN 0x00FFAA00
#define CONSOLE_COLOR_ERROR 0x00FF4040
#define CONSOLE_COLOR_PASSED 0x0040FF40

#define SERIAL_DATA  (COM1 + 0)
#define SERIAL_IER   (COM1 + 1)
#define SERIAL_FIFO  (COM1 + 2)
#define SERIAL_LCR   (COM1 + 3)
#define SERIAL_MCR   (COM1 + 4)
#define SERIAL_LSR   (COM1 + 5)

static char console_history[CONSOLE_HISTORY_SIZE];
static uint32_t console_history_colors[CONSOLE_HISTORY_SIZE];
static uint32_t console_history_next;
static uint32_t console_history_count;
static uint32_t current_console_color = CONSOLE_COLOR_DEFAULT;
static char systemd_log[SYSTEMD_LOG_SIZE];
static uint32_t systemd_log_next;
static uint32_t systemd_log_count;
static bool systemd_log_enabled;
static bool install_progress_active;
static uint32_t install_progress_percent;
static bool serial_initialized;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));

    return value;
}

static void console_history_append(char c)
{
    console_history[console_history_next] = c;
    console_history_colors[console_history_next] = current_console_color;
    console_history_next =
        (console_history_next + 1) % CONSOLE_HISTORY_SIZE;
    if (console_history_count < CONSOLE_HISTORY_SIZE)
        console_history_count++;
}

bool serial_init(void)
{
    outb(SERIAL_IER, 0x00);

    /* Enable divisor latch */
    outb(SERIAL_LCR, 0x80);

    /* 115200 baud */
    outb(SERIAL_DATA, 0x01);
    outb(SERIAL_IER, 0x00);

    /* 8 data bits, no parity, 1 stop bit */
    outb(SERIAL_LCR, 0x03);

    /* Enable FIFO */
    outb(SERIAL_FIFO, 0xC7);

    /* Enable DTR, RTS and OUT2 */
    outb(SERIAL_MCR, 0x0B);

    serial_initialized = true;
    return true;
}

static bool serial_transmit_char(char c)
{
    for (uint32_t i = 0; i < 100000; i++)
    {
        if (inb(SERIAL_LSR) & 0x20)
        {
            outb(SERIAL_DATA, (uint8_t)c);
            return true;
        }
    }

    return false;
}

void serial_putc_uart(char c)
{
    if (!serial_initialized)
        return;

    if (c == '\n')
        (void)serial_transmit_char('\r');
    (void)serial_transmit_char(c);
}

void serial_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');

    if (!serial_transmit_char(c))
        return;

    console_history_append(c);

    if (install_progress_active)
        return;
    vesa_console_set_color(current_console_color);
    vesa_console_putc(c);
}

void serial_console_replay(void)
{
    if (install_progress_active)
        return;
    vesa_console_batch_begin();
    uint32_t first =
        (console_history_next + CONSOLE_HISTORY_SIZE -
         console_history_count) % CONSOLE_HISTORY_SIZE;
    uint32_t replay_count = console_history_count;
    uint64_t height = vesa_get_height();
    uint64_t visible_rows = height > 8 ? (height - 8) / 16 : 0;
    if (visible_rows > 1)
    {
        uint64_t line_breaks = 0;
        for (uint32_t i = 0; i < console_history_count; i++)
        {
            uint32_t index = (first + i) % CONSOLE_HISTORY_SIZE;
            if (console_history[index] == '\n')
                line_breaks++;
        }

        uint64_t skipped_lines =
            line_breaks > visible_rows - 1
                ? line_breaks - (visible_rows - 1)
                : 0;
        if (skipped_lines)
        {
            for (uint32_t i = 0; i < console_history_count; i++)
            {
                uint32_t index = (first + i) % CONSOLE_HISTORY_SIZE;
                if (console_history[index] != '\n')
                    continue;
                if (--skipped_lines == 0)
                {
                    uint32_t skipped = i + 1;
                    first = (first + skipped) % CONSOLE_HISTORY_SIZE;
                    replay_count -= skipped;
                    break;
                }
            }
        }
    }

    vesa_console_clear();
    for (uint32_t i = 0; i < replay_count; i++)
    {
        uint32_t index = (first + i) % CONSOLE_HISTORY_SIZE;
        vesa_console_set_color(console_history_colors[index]);
        vesa_console_putc(console_history[index]);
    }
    vesa_console_set_color(CONSOLE_COLOR_DEFAULT);
    current_console_color = CONSOLE_COLOR_DEFAULT;
    vesa_console_batch_end();
}

void serial_console_reset_cursor(void)
{
    vesa_console_reset();
}

void serial_console_set_color(uint32_t color)
{
    current_console_color = color & 0x00FFFFFF;
    vesa_console_set_color(current_console_color);
}

void serial_systemd_log_control(bool enabled)
{
    if (enabled && !systemd_log_enabled)
    {
        systemd_log_next = 0;
        systemd_log_count = 0;
    }
    systemd_log_enabled = enabled;
}

void serial_systemd_log_write(const char *buffer, uint64_t size)
{
    if (!systemd_log_enabled || (!buffer && size))
        return;

    for (uint64_t i = 0; i < size; i++)
    {
        systemd_log[systemd_log_next] = buffer[i];
        systemd_log_next = (systemd_log_next + 1) % SYSTEMD_LOG_SIZE;
        if (systemd_log_count < SYSTEMD_LOG_SIZE)
            systemd_log_count++;
    }
}

uint64_t serial_systemd_log_size(void)
{
    return systemd_log_count;
}

uint64_t serial_systemd_log_read(
    uint64_t offset,
    void *buffer,
    uint64_t size
)
{
    if (!buffer && size)
        return 0;
    if (offset >= systemd_log_count)
        return 0;
    if (size > systemd_log_count - offset)
        size = systemd_log_count - offset;

    uint32_t first =
        (systemd_log_next + SYSTEMD_LOG_SIZE - systemd_log_count) %
        SYSTEMD_LOG_SIZE;
    for (uint64_t i = 0; i < size; i++)
    {
        uint64_t index = (first + offset + i) % SYSTEMD_LOG_SIZE;
        ((char *)buffer)[i] = systemd_log[index];
    }
    return size;
}

void serial_console_write(const char *buffer, uint64_t size)
{
    if (!buffer && size)
        return;
    if (install_progress_active)
    {
        for (uint64_t i = 0; i < size; i++)
            console_history_append(buffer[i]);
        return;
    }
    vesa_console_batch_begin();
    vesa_console_set_color(current_console_color);
    for (uint64_t i = 0; i < size; i++)
    {
        console_history_append(buffer[i]);
        vesa_console_putc(buffer[i]);
    }
    vesa_console_batch_end();
}

static void install_progress_write(const char *text)
{
    while (*text)
        vesa_console_putc(*text++);
}

static void install_progress_write_percent(uint32_t percent)
{
    char digits[3];
    uint32_t count = 0;
    do
    {
        digits[count++] = (char)('0' + percent % 10);
        percent /= 10;
    } while (percent && count < sizeof(digits));

    while (count)
        vesa_console_putc(digits[--count]);
}

static void install_progress_draw(uint32_t percent, const char *error)
{
    static const uint32_t half_width = 24;
    uint32_t filled = percent / 2;
    uint32_t left_filled = filled < half_width ? filled : half_width;
    uint32_t right_filled = filled > half_width
        ? filled - half_width
        : 0;
    if (right_filled > half_width)
        right_filled = half_width;

    vesa_console_batch_begin();
    vesa_console_clear();
    vesa_console_set_color(CONSOLE_COLOR_DEFAULT);
    install_progress_write("Installing please wait\n[");
    for (uint32_t i = 0; i < half_width; i++)
        vesa_console_putc(i < left_filled ? '#' : '_');
    install_progress_write_percent(percent);
    install_progress_write("%");
    for (uint32_t i = 0; i < half_width; i++)
        vesa_console_putc(i < right_filled ? '#' : '_');
    install_progress_write("]\n");
    if (error)
    {
        install_progress_write("Error: ");
        install_progress_write(error);
        vesa_console_putc('\n');
    }
    vesa_console_batch_end();
}

void serial_install_progress_begin(void)
{
    install_progress_active = true;
    install_progress_percent = 0;
    install_progress_draw(0, NULL);
}

void serial_install_progress_update(uint32_t percent)
{
    if (!install_progress_active)
        return;
    if (percent > 100)
        percent = 100;
    percent -= percent % 2;
    if (percent <= install_progress_percent)
        return;
    install_progress_percent = percent;
    install_progress_draw(percent, NULL);
}

void serial_install_progress_error(const char *message)
{
    if (!install_progress_active)
        return;
    install_progress_draw(install_progress_percent, message);
    install_progress_active = false;
}

void serial_install_progress_complete(void)
{
    if (!install_progress_active)
        return;
    serial_install_progress_update(100);
    install_progress_active = false;
}

uint64_t serial_boot_log_size(void)
{
    return console_history_count;
}

uint64_t serial_boot_log_read(
    uint64_t offset,
    void *buffer,
    uint64_t size
)
{
    if (!buffer && size)
        return 0;
    if (offset >= console_history_count)
        return 0;
    if (size > console_history_count - offset)
        size = console_history_count - offset;

    uint32_t first =
        (console_history_next + CONSOLE_HISTORY_SIZE -
         console_history_count) % CONSOLE_HISTORY_SIZE;
    uint64_t copied = 0;
    while (copied < size)
    {
        uint64_t index =
            (first + offset + copied) % CONSOLE_HISTORY_SIZE;
        uint64_t amount = size - copied;
        uint64_t contiguous = CONSOLE_HISTORY_SIZE - index;
        if (amount > contiguous)
            amount = contiguous;
        for (uint64_t i = 0; i < amount; i++)
            ((char *)buffer)[copied + i] = console_history[index + i];
        copied += amount;
    }
    return copied;
}

void serial_write(const char *str)
{
    if (!str)
        return;

    while (*str)
        serial_putc(*str++);
}

void serial_write_dec(uint64_t value)
{
    char buffer[21];
    uint32_t i = 0;

    if (value == 0)
    {
        serial_putc('0');
        return;
    }

    while (value > 0)
    {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }

    while (i > 0)
        serial_putc(buffer[--i]);
}

void serial_write_hex(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    serial_write("0x");

    bool started = false;

    for (int i = 15; i >= 0; i--)
    {
        uint8_t digit = (value >> (i * 4)) & 0xF;

        if (digit || started || i == 0)
        {
            serial_putc(digits[digit]);
            started = true;
        }
    }
}

static const char *log_level_name(serial_log_level_t level)
{
    switch (level)
    {
        case SERIAL_LOG_DEBUG: return "DEBUG";
        case SERIAL_LOG_INFO:  return "INFO ";
        case SERIAL_LOG_WARN:  return "WARN ";
        case SERIAL_LOG_ERROR: return "ERROR";
        default:               return "?????";
    }
}

static uint32_t log_level_color(serial_log_level_t level)
{
    switch (level)
    {
        case SERIAL_LOG_DEBUG: return CONSOLE_COLOR_DEBUG;
        case SERIAL_LOG_INFO:  return CONSOLE_COLOR_INFO;
        case SERIAL_LOG_WARN:  return CONSOLE_COLOR_WARN;
        case SERIAL_LOG_ERROR: return CONSOLE_COLOR_ERROR;
        default:               return CONSOLE_COLOR_DEFAULT;
    }
}

static bool message_is_passed(const char *message)
{
    if (!message)
        return false;
    static const char *success_words[] = {
        "PASSED", "passed", "succeeded", "successfully"
    };
    for (uint32_t word = 0;
         word < sizeof(success_words) / sizeof(success_words[0]);
         word++)
    {
        const char *start = message;
        while (*start)
        {
            uint32_t index = 0;
            while (success_words[word][index] &&
                   start[index] == success_words[word][index])
                index++;
            if (!success_words[word][index])
                return true;
            start++;
        }
    }
    return false;
}

static void serial_log_begin_with_color(
    serial_log_level_t level,
    uint32_t color
)
{
    vesa_console_batch_begin();
    current_console_color = color;
    vesa_console_set_color(color);
    uint64_t milliseconds = timer_get_milliseconds();

    serial_putc('[');
    serial_write_dec(milliseconds);
    serial_write(" ms] [");

    serial_write(log_level_name(level));

    serial_write("] ");
}

void serial_log_begin(serial_log_level_t level)
{
    serial_log_begin_with_color(level, log_level_color(level));
}

void serial_log_end(void)
{
    serial_putc('\n');
    current_console_color = CONSOLE_COLOR_DEFAULT;
    vesa_console_set_color(CONSOLE_COLOR_DEFAULT);
    vesa_console_batch_end();
}

void serial_log(serial_log_level_t level, const char *message)
{
    uint32_t color = message_is_passed(message)
        ? CONSOLE_COLOR_PASSED
        : log_level_color(level);
    serial_log_begin_with_color(level, color);
    serial_write(message);
    serial_log_end();
}
