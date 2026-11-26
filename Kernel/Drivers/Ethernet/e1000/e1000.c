#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "e1000.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "vmm.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define E1000_RX_COUNT 16
#define E1000_TX_COUNT 16
#define E1000_BUFFER_SIZE 2048
#define E1000_BAR_SIZE 0x20000
#define E1000_MAX_FRAME 1518

#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_IMC 0x00D8
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_RAL 0x5400
#define REG_RAH 0x5404

#define CTRL_RST (1U << 26)
#define STATUS_LU (1U << 1)
#define RCTL_EN (1U << 1)
#define RCTL_BAM (1U << 15)
#define RCTL_SECRC (1U << 26)
#define TCTL_EN (1U << 1)
#define TCTL_PSP (1U << 3)
#define TX_CMD_EOP 0x01
#define TX_CMD_IFCS 0x02
#define TX_CMD_RS 0x08
#define RX_STATUS_DD 0x01
#define RX_STATUS_EOP 0x02
#define TX_STATUS_DD 0x01

typedef struct
{
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_descriptor_t;

typedef struct
{
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
} __attribute__((packed)) e1000_tx_descriptor_t;

static volatile uint8_t *registers;
static e1000_rx_descriptor_t *rx_ring;
static e1000_tx_descriptor_t *tx_ring;
static uint8_t *rx_buffers[E1000_RX_COUNT];
static uint8_t *tx_buffers[E1000_TX_COUNT];
static uint64_t rx_ring_physical;
static uint64_t tx_ring_physical;
static uint32_t rx_index;
static uint32_t tx_index;
static bool tx_busy;
static uint8_t mac_address[6];
static bool initialized;

static void memory_zero(void *destination, size_t length)
{
    volatile uint8_t *bytes = destination;
    for (size_t i = 0; i < length; i++)
        bytes[i] = 0;
}

static void memory_copy(void *destination, const void *source, size_t length)
{
    volatile uint8_t *output = destination;
    const volatile uint8_t *input = source;
    for (size_t i = 0; i < length; i++)
        output[i] = input[i];
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function,
    uint8_t offset)
{
    uint32_t address = 0x80000000U |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write16(uint8_t bus, uint8_t device, uint8_t function,
    uint8_t offset, uint16_t value)
{
    uint32_t address = 0x80000000U |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    outw(PCI_CONFIG_DATA + (offset & 2), value);
}

static bool find_controller(uint8_t *bus_out, uint8_t *device_out,
    uint8_t *function_out, uint32_t *bar_out)
{
    for (uint32_t bus = 0; bus < 256; bus++)
    {
        for (uint32_t device = 0; device < 32; device++)
        {
            uint32_t id0 = pci_read32(bus, device, 0, 0);
            uint16_t vendor = (uint16_t)id0;
            uint16_t device_id = (uint16_t)(id0 >> 16);
            if (vendor == 0xFFFF)
                continue;

            uint32_t header = pci_read32(bus, device, 0, 0x0C);
            uint32_t functions = (header & 0x00800000U) ? 8 : 1;
            for (uint32_t function = 0; function < functions; function++)
            {
                uint32_t id = function
                    ? pci_read32(bus, device, function, 0)
                    : id0;
                vendor = (uint16_t)id;
                device_id = (uint16_t)(id >> 16);
                if (vendor != 0x8086 ||
                    (device_id != 0x100E &&
                     device_id != 0x100F &&
                     device_id != 0x10D3 &&
                     device_id != 0x10EA))
                    continue;

                uint32_t class_code =
                    pci_read32(bus, device, function, 0x08);
                if ((class_code >> 24) != 0x02)
                    continue;

                uint32_t bar = pci_read32(bus, device, function, 0x10);
                if ((bar & 1) || (bar & 0x6) == 0x4)
                    continue;
                *bus_out = bus;
                *device_out = device;
                *function_out = function;
                *bar_out = bar & ~0xFU;
                return *bar_out != 0;
            }
        }
    }
    return false;
}

static uint32_t register_read(uint32_t offset)
{
    return *(volatile uint32_t *)(registers + offset);
}

static void register_write(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(registers + offset) = value;
}

static uint64_t allocate_dma_page(void **virtual_address)
{
    uint64_t physical = pmm_alloc_page();
    if (!physical)
        return 0;
    if (physical > UINT32_MAX - 4095)
    {
        pmm_free_page(physical);
        return 0;
    }
    void *memory = paging_phys_to_virt(physical);
    if (!memory)
    {
        pmm_free_page(physical);
        return 0;
    }
    memory_zero(memory, VMM_PAGE_SIZE);
    *virtual_address = memory;
    return physical;
}

bool e1000_init(void)
{
    initialized = false;
    uint8_t bus, device, function;
    uint32_t bar;
    if (!find_controller(&bus, &device, &function, &bar))
        return false;

    uint32_t command_status = pci_read32(bus, device, function, 0x04);
    uint16_t command = (uint16_t)command_status;
    command |= (1U << 1) | (1U << 2);
    pci_write16(bus, device, function, 0x04, command);

    registers = ioremap(bar, E1000_BAR_SIZE);
    if (!registers)
        return false;

    register_write(REG_IMC, UINT32_MAX);
    register_write(REG_CTRL, register_read(REG_CTRL) | CTRL_RST);
    for (volatile uint32_t delay = 0; delay < 1000000; delay++)
        __asm__ volatile ("pause");
    register_write(REG_IMC, UINT32_MAX);

    uint32_t ral = register_read(REG_RAL);
    uint32_t rah = register_read(REG_RAH);
    for (uint32_t i = 0; i < 4; i++)
        mac_address[i] = (uint8_t)(ral >> (i * 8));
    mac_address[4] = (uint8_t)rah;
    mac_address[5] = (uint8_t)(rah >> 8);
    if (!(rah & (1U << 31)))
        return false;

    rx_ring_physical = allocate_dma_page((void **)&rx_ring);
    tx_ring_physical = allocate_dma_page((void **)&tx_ring);
    if (!rx_ring_physical || !tx_ring_physical)
        return false;

    for (uint32_t i = 0; i < E1000_RX_COUNT; i++)
    {
        uint64_t physical =
            allocate_dma_page((void **)&rx_buffers[i]);
        if (!physical)
            return false;
        rx_ring[i].address = physical;
    }
    for (uint32_t i = 0; i < E1000_TX_COUNT; i++)
    {
        uint64_t physical =
            allocate_dma_page((void **)&tx_buffers[i]);
        if (!physical)
            return false;
        tx_ring[i].address = physical;
        tx_ring[i].status = TX_STATUS_DD;
    }

    register_write(REG_RDBAL, (uint32_t)rx_ring_physical);
    register_write(REG_RDBAH, (uint32_t)(rx_ring_physical >> 32));
    register_write(REG_RDLEN, E1000_RX_COUNT * sizeof(*rx_ring));
    register_write(REG_RDH, 0);
    register_write(REG_RDT, E1000_RX_COUNT - 1);
    register_write(
        REG_RCTL,
        RCTL_EN | RCTL_BAM | RCTL_SECRC);

    register_write(REG_TDBAL, (uint32_t)tx_ring_physical);
    register_write(REG_TDBAH, (uint32_t)(tx_ring_physical >> 32));
    register_write(REG_TDLEN, E1000_TX_COUNT * sizeof(*tx_ring));
    register_write(REG_TDH, 0);
    register_write(REG_TDT, 0);
    register_write(REG_TIPG, 0x0060200A);
    register_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10 << 4) |
        (0x40 << 12));
    rx_index = 0;
    tx_index = 0;
    initialized = true;

    serial_log(SERIAL_LOG_INFO, "Initialized QEMU-compatible Intel e1000 NIC");
    return true;
}

bool e1000_is_initialized(void)
{
    return initialized;
}

bool e1000_link_is_up(void)
{
    return initialized && (register_read(REG_STATUS) & STATUS_LU);
}

const uint8_t *e1000_mac_address(void)
{
    return initialized ? mac_address : NULL;
}

bool e1000_send(const void *packet, size_t length)
{
    if (!initialized || !packet || length < 14 ||
        length > E1000_MAX_FRAME)
        return false;

    for (;;)
    {
        uint64_t interrupt_flags;
        __asm__ volatile (
            "pushfq\n\t"
            "popq %0\n\t"
            "cli"
            : "=r"(interrupt_flags)
            :
            : "memory"
        );
        if (!tx_busy)
        {
            tx_busy = true;
            if (interrupt_flags & (1ULL << 9))
                __asm__ volatile ("sti" ::: "memory");
            break;
        }
        if (interrupt_flags & (1ULL << 9))
            __asm__ volatile ("sti" ::: "memory");
        return false;
    }

    e1000_tx_descriptor_t *descriptor = &tx_ring[tx_index];
    for (uint32_t timeout = 0;
         timeout < 1000000 && !(descriptor->status & TX_STATUS_DD);
         timeout++)
        __asm__ volatile ("pause");
    if (!(descriptor->status & TX_STATUS_DD))
    {
        tx_busy = false;
        return false;
    }

    memory_copy(tx_buffers[tx_index], packet, length);
    descriptor->length = (uint16_t)length;
    descriptor->command = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    descriptor->status = 0;
    tx_index = (tx_index + 1) % E1000_TX_COUNT;
    register_write(REG_TDT, tx_index);
    tx_busy = false;
    return true;
}

size_t e1000_receive(void *packet, size_t capacity)
{
    if (!initialized || !packet || capacity == 0)
        return 0;
    e1000_rx_descriptor_t *descriptor = &rx_ring[rx_index];
    if (!(descriptor->status & RX_STATUS_DD) ||
        !(descriptor->status & RX_STATUS_EOP))
        return 0;

    size_t length = descriptor->length;
    if (length > capacity)
        length = 0;
    if (length)
        memory_copy(packet, rx_buffers[rx_index], length);

    descriptor->length = 0;
    descriptor->status = 0;
    descriptor->errors = 0;
    register_write(REG_RDT, rx_index);
    rx_index = (rx_index + 1) % E1000_RX_COUNT;
    return length;
}
