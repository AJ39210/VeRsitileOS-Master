#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "rtl810x.h"
#include "serial.h"
#include "vmm.h"

#define RTL810X_VENDOR_ID 0x10EC
#define RTL810X_DEVICE_ID 0x8136
#define RTL810X_REGISTER_SIZE 0x100
#define RTL810X_DESCRIPTOR_COUNT 64
#define RTL810X_BUFFER_SIZE 2048
#define RTL810X_MAX_FRAME_SIZE 1518
#define RTL810X_MIN_FRAME_SIZE 60
#define RTL810X_PCI_CLASS_NETWORK 0x02
#define RTL810X_PCI_SUBCLASS_ETHERNET 0x00

#define REG_MAC 0x00
#define REG_TX_DESC_LOW 0x20
#define REG_TX_DESC_HIGH 0x24
#define REG_CHIP_COMMAND 0x37
#define REG_TX_POLL 0x38
#define REG_INTERRUPT_MASK 0x3C
#define REG_TX_CONFIG 0x40
#define REG_RX_CONFIG 0x44
#define REG_CFG9346 0x50
#define REG_CONFIG1 0x52
#define REG_CONFIG3 0x54
#define REG_PHY_STATUS 0x6C
#define REG_CSIDR 0x64
#define REG_CSIAR 0x68
#define REG_PHYAR 0x60
#define REG_RX_MAX_SIZE 0xDA
#define REG_CPLUS_CMD 0xE0
#define REG_RX_DESC_LOW 0xE4
#define REG_RX_DESC_HIGH 0xE8
#define REG_EARLY_TX_THRESHOLD 0xEC
#define REG_EPHYAR 0x80
#define REG_DEBUG 0xD1

#define CHIP_COMMAND_RESET 0x10
#define CHIP_COMMAND_RX_ENABLE 0x08
#define CHIP_COMMAND_TX_ENABLE 0x04
#define CFG9346_UNLOCK 0xC0
#define CFG9346_LOCK 0x00
#define TX_POLL_NORMAL_PRIORITY 0x40
#define PHY_STATUS_LINK 0x02
#define CSI_FLAG (1U << 31)
#define CSI_WRITE (1U << 31)
#define CSI_BYTE_ENABLE (0x0FU << 12)
#define EPHY_FLAG (1U << 31)
#define EPHY_WRITE (1U << 31)
#define PHY_FLAG (1U << 31)
#define CPLUS_PCIMULRW (1U << 3)
#define CPLUS_RTL810X_QUIRKS 0xDF98U
#define RTL810X_RX_CONFIG ((7U << 13) | (6U << 8) | 0x0EU)
#define RTL810X_TX_CONFIG ((3U << 24) | (6U << 8))
#define CONFIG1_LEDS0 (1U << 6)
#define CONFIG1_LEDS1 (1U << 7)
#define CONFIG1_SPEED_DOWN (1U << 4)
#define CONFIG1_MEMMAP (1U << 3)
#define CONFIG1_IOMAP (1U << 2)
#define CONFIG1_VPD (1U << 1)
#define CONFIG1_PM_ENABLE (1U << 0)
#define CONFIG3_BEACON_ENABLE (1U << 0)
#define EPHY_TIMEOUT 100000
#define PHY_TIMEOUT 100000
#define CSI_TIMEOUT 100000

#define DESC_OWN (1U << 31)
#define DESC_END_OF_RING (1U << 30)
#define DESC_FIRST_SEGMENT (1U << 29)
#define DESC_LAST_SEGMENT (1U << 28)
#define DESC_LENGTH_MASK 0x3FFFU
#define RX_STATUS_ERROR_MASK 0x00F80000U

typedef struct __attribute__((packed))
{
    volatile uint32_t status;
    uint32_t options;
    uint64_t address;
} rtl810x_descriptor_t;

static volatile uint8_t *registers;
static rtl810x_descriptor_t *rx_ring;
static rtl810x_descriptor_t *tx_ring;
static uint8_t *rx_buffers[RTL810X_DESCRIPTOR_COUNT];
static uint8_t *tx_buffers[RTL810X_DESCRIPTOR_COUNT];
static uint64_t rx_ring_physical;
static uint64_t tx_ring_physical;
static uint64_t rx_buffer_physical[RTL810X_DESCRIPTOR_COUNT];
static uint64_t tx_buffer_physical[RTL810X_DESCRIPTOR_COUNT];
static uint32_t rx_index;
static uint32_t tx_index;
static bool tx_busy;
static uint8_t mac_address[6];
static bool initialized;

static uint8_t register_read8(uint32_t offset)
{
    return registers[offset];
}

static uint32_t register_read32(uint32_t offset)
{
    return *(volatile uint32_t *)(registers + offset);
}

static void register_write8(uint32_t offset, uint8_t value)
{
    registers[offset] = value;
}

static void register_write16(uint32_t offset, uint16_t value)
{
    *(volatile uint16_t *)(registers + offset) = value;
}

static void register_write32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(registers + offset) = value;
}

static bool csi_wait(bool write)
{
    for (uint32_t timeout = 0; timeout < CSI_TIMEOUT; timeout++)
    {
        bool pending = (register_read32(REG_CSIAR) & CSI_FLAG) != 0;
        if (pending != write)
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool csi_read(uint16_t address, uint32_t *value)
{
    register_write32(
        REG_CSIAR,
        ((uint32_t)address & 0x0FFFU) | CSI_BYTE_ENABLE);
    if (!csi_wait(false))
        return false;
    *value = register_read32(REG_CSIDR);
    return true;
}

static bool csi_write(uint16_t address, uint32_t value)
{
    register_write32(REG_CSIDR, value);
    register_write32(
        REG_CSIAR,
        CSI_WRITE | ((uint32_t)address & 0x0FFFU) | CSI_BYTE_ENABLE);
    return csi_wait(true);
}

static bool ephy_write(uint8_t address, uint16_t value)
{
    register_write32(
        REG_EPHYAR,
        EPHY_WRITE | ((uint32_t)(address & 0x1FU) << 16) | value);
    for (uint32_t timeout = 0; timeout < EPHY_TIMEOUT; timeout++)
    {
        if (!(register_read32(REG_EPHYAR) & EPHY_FLAG))
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool phy_write(uint8_t address, uint16_t value)
{
    register_write32(
        REG_PHYAR,
        PHY_FLAG | ((uint32_t)(address & 0x1FU) << 16) | value);
    for (uint32_t timeout = 0; timeout < PHY_TIMEOUT; timeout++)
    {
        if (!(register_read32(REG_PHYAR) & PHY_FLAG))
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool phy_read(uint8_t address, uint16_t *value)
{
    register_write32(
        REG_PHYAR,
        (uint32_t)(address & 0x1FU) << 16);
    for (uint32_t timeout = 0; timeout < PHY_TIMEOUT; timeout++)
    {
        uint32_t result = register_read32(REG_PHYAR);
        if (result & PHY_FLAG)
        {
            *value = (uint16_t)result;
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

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

static void memory_barrier(void)
{
    __asm__ volatile ("mfence" ::: "memory");
}

static uint64_t allocate_dma_page(void **virtual_address)
{
    uint64_t physical = pmm_alloc_page();
    if (!physical)
        return 0;

    void *memory = paging_phys_to_virt(physical);
    if (!memory)
    {
        (void)pmm_free_page(physical);
        return 0;
    }

    memory_zero(memory, VMM_PAGE_SIZE);
    *virtual_address = memory;
    return physical;
}

static void release_dma_resources(void)
{
    if (rx_ring_physical)
        (void)pmm_free_page(rx_ring_physical);
    if (tx_ring_physical)
        (void)pmm_free_page(tx_ring_physical);
    rx_ring_physical = 0;
    tx_ring_physical = 0;
    rx_ring = NULL;
    tx_ring = NULL;

    for (uint32_t i = 0; i < RTL810X_DESCRIPTOR_COUNT; i++)
    {
        if (rx_buffer_physical[i])
            (void)pmm_free_page(rx_buffer_physical[i]);
        if (tx_buffer_physical[i])
            (void)pmm_free_page(tx_buffer_physical[i]);
        rx_buffer_physical[i] = 0;
        tx_buffer_physical[i] = 0;
        rx_buffers[i] = NULL;
        tx_buffers[i] = NULL;
    }
}

static bool mac_address_is_valid(void)
{
    bool all_zero = true;
    bool all_ones = true;
    for (uint32_t i = 0; i < sizeof(mac_address); i++)
    {
        all_zero = all_zero && mac_address[i] == 0;
        all_ones = all_ones && mac_address[i] == 0xFF;
    }
    return !all_zero && !all_ones && !(mac_address[0] & 1);
}

static const pci_device_t *find_controller(pci_bar_info_t *mmio_bar)
{
    for (uint32_t i = 0; i < pci_device_count(); i++)
    {
        const pci_device_t *device = pci_device_get(i);
        if (!device || device->vendor_id != RTL810X_VENDOR_ID ||
            device->device_id != RTL810X_DEVICE_ID ||
            device->class_code != RTL810X_PCI_CLASS_NETWORK ||
            device->subclass != RTL810X_PCI_SUBCLASS_ETHERNET)
            continue;

        for (uint8_t bar_index = 0;
             bar_index < device->bar_count && bar_index < PCI_BAR_LIMIT;
             bar_index++)
        {
            pci_bar_info_t bar;
            if (!pci_get_bar_info(device, bar_index, &bar) ||
                bar.is_io || bar.size < RTL810X_REGISTER_SIZE ||
                !bar.base)
                continue;

            *mmio_bar = bar;
            return device;
        }
    }
    return NULL;
}

static bool allocate_rings(void)
{
    rx_ring_physical = allocate_dma_page((void **)&rx_ring);
    if (!rx_ring_physical)
        goto failed;
    tx_ring_physical = allocate_dma_page((void **)&tx_ring);
    if (!tx_ring_physical)
        goto failed;

    for (uint32_t i = 0; i < RTL810X_DESCRIPTOR_COUNT; i++)
    {
        rx_buffer_physical[i] =
            allocate_dma_page((void **)&rx_buffers[i]);
        if (!rx_buffer_physical[i])
            goto failed;
        tx_buffer_physical[i] =
            allocate_dma_page((void **)&tx_buffers[i]);
        if (!tx_buffer_physical[i])
            goto failed;

        rx_ring[i].address = rx_buffer_physical[i];
        rx_ring[i].status = DESC_OWN | RTL810X_BUFFER_SIZE;
        tx_ring[i].address = tx_buffer_physical[i];
    }

    rx_ring[RTL810X_DESCRIPTOR_COUNT - 1].status |= DESC_END_OF_RING;
    tx_ring[RTL810X_DESCRIPTOR_COUNT - 1].status = DESC_END_OF_RING;
    return true;

failed:
    release_dma_resources();
    return false;
}

static bool reset_controller(void)
{
    register_write8(REG_CHIP_COMMAND, CHIP_COMMAND_RESET);
    for (uint32_t timeout = 0; timeout < 1000000; timeout++)
    {
        if (!(register_read8(REG_CHIP_COMMAND) & CHIP_COMMAND_RESET))
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool configure_phy(void)
{
    uint16_t value;
    if (!phy_write(0x1F, 0x0000) ||
        !phy_read(0x11, &value) ||
        !phy_write(0x11, value | (1U << 12)) ||
        !phy_read(0x19, &value) ||
        !phy_write(0x19, value | (1U << 13)) ||
        !phy_write(0x1F, 0x0003) ||
        !phy_write(0x08, 0x441D) ||
        !phy_write(0x01, 0x9100))
        return false;
    return phy_write(0x1F, 0x0000);
}

static bool configure_chip(uint8_t mac_version)
{
    uint32_t csi_value;
    if (!csi_read(0x070C, &csi_value) ||
        !csi_write(0x070C, (csi_value & 0x00FFFFFFU) | 0x27000000U))
        return false;

    uint8_t config1;
    if (mac_version == 7)
    {
        register_write8(REG_DEBUG, register_read8(REG_DEBUG) | (1U << 4));
        config1 = CONFIG1_LEDS1 | CONFIG1_LEDS0 |
            CONFIG1_SPEED_DOWN | CONFIG1_MEMMAP | CONFIG1_IOMAP |
            CONFIG1_VPD | CONFIG1_PM_ENABLE;
        if ((config1 & (CONFIG1_LEDS0 | CONFIG1_LEDS1)) ==
            (CONFIG1_LEDS0 | CONFIG1_LEDS1))
            config1 &= (uint8_t)~CONFIG1_LEDS0;
    }
    else
    {
        config1 = CONFIG1_MEMMAP | CONFIG1_IOMAP |
            CONFIG1_VPD | CONFIG1_PM_ENABLE;
    }
    register_write8(REG_CONFIG1, config1);
    register_write8(
        REG_CONFIG3,
        register_read8(REG_CONFIG3) & (uint8_t)~CONFIG3_BEACON_ENABLE);

    uint16_t cplus = *(volatile uint16_t *)(registers + REG_CPLUS_CMD);
    cplus = (uint16_t)((cplus & (uint16_t)~CPLUS_RTL810X_QUIRKS) |
                       CPLUS_PCIMULRW);
    register_write16(REG_CPLUS_CMD, cplus);

    if (mac_version == 7)
    {
        static const struct
        {
            uint8_t address;
            uint16_t value;
        } ephy_settings[] = {
            { 0x01, 0x6E65 },
            { 0x02, 0x091F },
            { 0x03, 0xC2F9 },
            { 0x06, 0xAFB5 },
            { 0x07, 0x0E00 },
            { 0x19, 0xEC80 },
            { 0x01, 0x2E65 },
            { 0x01, 0x6E65 }
        };
        for (size_t i = 0; i < sizeof(ephy_settings) / sizeof(ephy_settings[0]); i++)
        {
            if (!ephy_write(
                    ephy_settings[i].address,
                    ephy_settings[i].value))
                return false;
        }
    }
    else if (mac_version == 8 && !ephy_write(0x03, 0xC2F9))
        return false;

    return configure_phy();
}

bool rtl810x_init(void)
{
    if (initialized)
        return true;

    pci_bar_info_t bar;
    const pci_device_t *device = find_controller(&bar);
    if (!device || !pci_enable_device(device, true, true))
        return false;

    registers = ioremap(bar.base, RTL810X_REGISTER_SIZE);
    if (!registers)
        return false;

    uint32_t tx_config = register_read32(REG_TX_CONFIG);
    uint8_t mac_version = rtl810x_mac_version(tx_config);
    if (!mac_version ||
        (!rtl8102e_matches_tx_config(tx_config) &&
         !rtl8103e_matches_tx_config(tx_config)))
        return false;

    register_write16(REG_INTERRUPT_MASK, 0);
    if (!reset_controller())
        return false;
    register_write16(REG_INTERRUPT_MASK, 0);

    register_write8(REG_CFG9346, CFG9346_UNLOCK);
    if (!configure_chip(mac_version))
    {
        register_write8(REG_CFG9346, CFG9346_LOCK);
        return false;
    }
    register_write8(REG_CFG9346, CFG9346_LOCK);

    for (uint32_t i = 0; i < sizeof(mac_address); i++)
        mac_address[i] = register_read8(REG_MAC + i);
    if (!mac_address_is_valid())
        return false;

    if (!allocate_rings())
        return false;

    register_write8(REG_CFG9346, CFG9346_UNLOCK);
    register_write8(REG_EARLY_TX_THRESHOLD, 0x3F);
    register_write32(REG_TX_DESC_LOW, (uint32_t)tx_ring_physical);
    register_write32(REG_TX_DESC_HIGH, (uint32_t)(tx_ring_physical >> 32));
    register_write32(REG_RX_DESC_LOW, (uint32_t)rx_ring_physical);
    register_write32(REG_RX_DESC_HIGH, (uint32_t)(rx_ring_physical >> 32));
    register_write16(REG_RX_MAX_SIZE, RTL810X_BUFFER_SIZE);
    register_write32(REG_RX_CONFIG, RTL810X_RX_CONFIG);
    register_write32(REG_TX_CONFIG, RTL810X_TX_CONFIG);
    register_write8(REG_CFG9346, CFG9346_LOCK);
    register_write8(REG_TX_POLL, 0);

    rx_index = 0;
    tx_index = 0;
    tx_busy = false;
    memory_barrier();
    register_write8(
        REG_CHIP_COMMAND,
        CHIP_COMMAND_RX_ENABLE | CHIP_COMMAND_TX_ENABLE);
    initialized = true;
    serial_log(
        SERIAL_LOG_INFO,
        mac_version == 9
            ? "Initialized Realtek RTL8103E Ethernet controller"
            : "Initialized Realtek RTL8102E Ethernet controller");
    return true;
}

bool rtl810x_is_initialized(void)
{
    return initialized;
}

bool rtl810x_link_is_up(void)
{
    return initialized && (register_read8(REG_PHY_STATUS) & PHY_STATUS_LINK);
}

const uint8_t *rtl810x_mac_address(void)
{
    return initialized ? mac_address : NULL;
}

bool rtl810x_send(const void *packet, size_t length)
{
    if (!initialized || !packet || length < 14 ||
        length > RTL810X_MAX_FRAME_SIZE)
        return false;

    uint64_t interrupt_flags;
    __asm__ volatile (
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(interrupt_flags)
        :
        : "memory"
    );
    if (tx_busy)
    {
        if (interrupt_flags & (1ULL << 9))
            __asm__ volatile ("sti" ::: "memory");
        return false;
    }
    tx_busy = true;
    if (interrupt_flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");

    uint32_t index = tx_index;
    rtl810x_descriptor_t *descriptor = &tx_ring[index];
    if (descriptor->status & DESC_OWN)
    {
        tx_busy = false;
        return false;
    }
    memory_barrier();

    size_t dma_length = length < RTL810X_MIN_FRAME_SIZE
        ? RTL810X_MIN_FRAME_SIZE
        : length;
    memory_copy(tx_buffers[index], packet, length);
    if (dma_length > length)
        memory_zero(tx_buffers[index] + length, dma_length - length);

    uint32_t end_of_ring = index == RTL810X_DESCRIPTOR_COUNT - 1
        ? DESC_END_OF_RING
        : 0;
    descriptor->options = 0;
    memory_barrier();
    descriptor->status = DESC_OWN | DESC_FIRST_SEGMENT |
        DESC_LAST_SEGMENT | end_of_ring | (uint32_t)dma_length;
    memory_barrier();
    tx_index = (index + 1) % RTL810X_DESCRIPTOR_COUNT;
    register_write8(REG_TX_POLL, TX_POLL_NORMAL_PRIORITY);
    tx_busy = false;
    return true;
}

size_t rtl810x_receive(void *packet, size_t capacity)
{
    if (!initialized || !packet || !capacity)
        return 0;

    rtl810x_descriptor_t *descriptor = &rx_ring[rx_index];
    uint32_t status = descriptor->status;
    if (status & DESC_OWN)
        return 0;
    memory_barrier();

    size_t length = status & DESC_LENGTH_MASK;
    if ((status & RX_STATUS_ERROR_MASK) ||
        !(status & DESC_FIRST_SEGMENT) ||
        !(status & DESC_LAST_SEGMENT) ||
        length < 4 || length > RTL810X_BUFFER_SIZE)
        length = 0;
    else
    {
        length -= 4;
        if (length > capacity)
            length = 0;
    }

    if (length)
        memory_copy(packet, rx_buffers[rx_index], length);

    uint32_t end_of_ring =
        rx_index == RTL810X_DESCRIPTOR_COUNT - 1
            ? DESC_END_OF_RING
            : 0;
    memory_barrier();
    descriptor->status = DESC_OWN | end_of_ring | RTL810X_BUFFER_SIZE;
    rx_index = (rx_index + 1) % RTL810X_DESCRIPTOR_COUNT;
    return length;
}
