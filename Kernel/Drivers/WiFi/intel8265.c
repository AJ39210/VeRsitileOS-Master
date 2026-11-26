#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "heap.h"
#include "intel8265.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "vfs.h"

#define INTEL_VENDOR_ID 0x8086
#define INTEL_8265_8275_DEVICE_ID 0x24FD
#define INTEL8265_MMIO_MIN_SIZE 0x1000U
#define INTEL8265_MMIO_MAX_SIZE 0x100000U
#define INTEL8265_DMA_ADDRESS_LIMIT (1ULL << 36)
#define INTEL8265_FIRMWARE_HEADER_SIZE 80U
#define INTEL8265_FIRMWARE_API_VERSION_OFFSET 72U
#define INTEL8265_FIRMWARE_BUILD_OFFSET 76U
#define INTEL8265_FIRMWARE_TLV_HEADER_SIZE 8U
#define INTEL8265_FIRMWARE_MAGIC 0x0A4C5749U

static const pci_device_t *devices[PCI_DEVICE_LIMIT];
static uint32_t device_count;
static uint8_t *firmware_image;
static uint64_t firmware_image_size;
static intel8265_firmware_info_t firmware_info;
static intel8265_resources_t resources;

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static uint32_t firmware_crc32(const uint8_t *data, uint64_t size)
{
    uint32_t crc = UINT32_MAX;
    for (uint64_t i = 0; i < size; i++)
    {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^
                (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

bool intel8265_firmware_crc32(
    const uint8_t *data,
    uint64_t size,
    uint32_t *checksum)
{
    if (!data || !checksum || !size ||
        size > INTEL8265_FIRMWARE_MAX_SIZE)
        return false;

    *checksum = firmware_crc32(data, size);
    return true;
}

static bool firmware_inspect_internal(
    const uint8_t *data,
    uint64_t size,
    intel8265_firmware_info_t *info,
    bool calculate_crc)
{
    if (!data || !info ||
        size < INTEL8265_FIRMWARE_HEADER_SIZE ||
        size > INTEL8265_FIRMWARE_MAX_SIZE ||
        read_le32(data) != 0 ||
        read_le32(data + 4) != INTEL8265_FIRMWARE_MAGIC ||
        read_le32(data + INTEL8265_FIRMWARE_API_VERSION_OFFSET) !=
            INTEL8265_FIRMWARE_API_VERSION)
        return false;

    uint64_t offset = INTEL8265_FIRMWARE_HEADER_SIZE;
    uint32_t tlv_count = 0;
    uint32_t runtime_section_count = 0;
    uint32_t init_section_count = 0;
    uint32_t wowlan_section_count = 0;
    uint64_t runtime_section_bytes = 0;
    uint64_t init_section_bytes = 0;
    uint64_t wowlan_section_bytes = 0;
    while (offset < size)
    {
        if (size - offset < INTEL8265_FIRMWARE_TLV_HEADER_SIZE)
            return false;

        const uint32_t type = read_le32(data + offset);
        const uint32_t length =
            read_le32(data + offset + sizeof(uint32_t));
        offset += INTEL8265_FIRMWARE_TLV_HEADER_SIZE;
        if (length > size - offset || (length & 3))
            return false;

        if (type == INTEL8265_TLV_SEC_RT ||
            type == INTEL8265_TLV_SEC_INIT ||
            type == INTEL8265_TLV_SEC_WOWLAN)
        {
            if (length <= sizeof(uint32_t))
                return false;
            const uint32_t device_address = read_le32(data + offset);
            const uint64_t section_size = length - sizeof(uint32_t);
            if ((section_size & 3) || section_size > UINT32_MAX ||
                section_size > (uint64_t)UINT32_MAX + 1 -
                    device_address)
                return false;
            if (type == INTEL8265_TLV_SEC_RT)
            {
                runtime_section_count++;
                runtime_section_bytes += section_size;
            }
            else if (type == INTEL8265_TLV_SEC_INIT)
            {
                init_section_count++;
                init_section_bytes += section_size;
            }
            else
            {
                wowlan_section_count++;
                wowlan_section_bytes += section_size;
            }
        }

        offset += length;
        if (tlv_count == UINT32_MAX)
            return false;
        tlv_count++;
    }

    if (!tlv_count || !runtime_section_count || !init_section_count)
        return false;
    info->api_version =
        read_le32(data + INTEL8265_FIRMWARE_API_VERSION_OFFSET);
    info->build = read_le32(data + INTEL8265_FIRMWARE_BUILD_OFFSET);
    info->tlv_count = tlv_count;
    info->runtime_section_count = runtime_section_count;
    info->init_section_count = init_section_count;
    info->wowlan_section_count = wowlan_section_count;
    info->runtime_section_bytes = runtime_section_bytes;
    info->init_section_bytes = init_section_bytes;
    info->wowlan_section_bytes = wowlan_section_bytes;
    info->image_crc32 = calculate_crc ? firmware_crc32(data, size) : 0;
    return true;
}

bool intel8265_firmware_inspect(
    const uint8_t *data,
    uint64_t size,
    intel8265_firmware_info_t *info)
{
    return firmware_inspect_internal(data, size, info, true);
}

bool intel8265_firmware_tlv_get(
    const uint8_t *data,
    uint64_t size,
    uint32_t index,
    intel8265_firmware_tlv_t *tlv)
{
    intel8265_firmware_info_t info;
    if (!tlv ||
        !firmware_inspect_internal(data, size, &info, false) ||
        index >= info.tlv_count)
        return false;

    uint64_t offset = INTEL8265_FIRMWARE_HEADER_SIZE;
    for (uint32_t current = 0; current <= index; current++)
    {
        const uint32_t type = read_le32(data + offset);
        const uint32_t length =
            read_le32(data + offset + sizeof(uint32_t));
        const uint64_t payload_offset =
            offset + INTEL8265_FIRMWARE_TLV_HEADER_SIZE;
        if (current == index)
        {
            *tlv = (intel8265_firmware_tlv_t){
                .type = type,
                .length = length,
                .data = data + payload_offset
            };
            return true;
        }
        offset = payload_offset + length;
    }
    return false;
}

bool intel8265_firmware_section_get(
    const uint8_t *data,
    uint64_t size,
    uint32_t index,
    intel8265_firmware_section_t *section)
{
    intel8265_firmware_tlv_t tlv;
    if (!section ||
        !intel8265_firmware_tlv_get(data, size, index, &tlv) ||
        (tlv.type != INTEL8265_TLV_SEC_RT &&
         tlv.type != INTEL8265_TLV_SEC_INIT &&
         tlv.type != INTEL8265_TLV_SEC_WOWLAN) ||
        tlv.length <= sizeof(uint32_t))
        return false;

    const uint32_t device_address = read_le32(tlv.data);
    const uint32_t section_size = tlv.length - sizeof(uint32_t);
    if ((section_size & 3) ||
        (uint64_t)section_size >
        (uint64_t)UINT32_MAX + 1 - device_address)
        return false;

    *section = (intel8265_firmware_section_t){
        .type = tlv.type,
        .device_address = device_address,
        .size = section_size,
        .data = tlv.data + sizeof(uint32_t)
    };
    return true;
}

bool intel8265_firmware_image_section_get(
    const uint8_t *data,
    uint64_t size,
    intel8265_firmware_image_t image,
    uint32_t section_index,
    intel8265_firmware_section_t *section)
{
    if (!section ||
        (image != INTEL8265_FIRMWARE_IMAGE_INIT &&
         image != INTEL8265_FIRMWARE_IMAGE_RUNTIME))
        return false;

    intel8265_firmware_info_t info;
    if (!firmware_inspect_internal(data, size, &info, false))
        return false;

    uint32_t found = 0;
    for (uint32_t i = 0; i < info.tlv_count; i++)
    {
        intel8265_firmware_section_t candidate;
        if (!intel8265_firmware_section_get(data, size, i, &candidate) ||
            candidate.type != (uint32_t)image)
            continue;
        if (found++ == section_index)
        {
            *section = candidate;
            return true;
        }
    }
    return false;
}

bool intel8265_firmware_image_plan(
    const uint8_t *data,
    uint64_t size,
    intel8265_firmware_image_t image,
    uint32_t chunk_capacity,
    uint64_t *payload_bytes,
    uint32_t *chunk_count)
{
    if (!payload_bytes || !chunk_count ||
        chunk_capacity < sizeof(uint32_t) || (chunk_capacity & 3) ||
        (image != INTEL8265_FIRMWARE_IMAGE_INIT &&
         image != INTEL8265_FIRMWARE_IMAGE_RUNTIME))
        return false;

    intel8265_firmware_info_t info;
    if (!firmware_inspect_internal(data, size, &info, false))
        return false;

    const uint32_t section_count =
        image == INTEL8265_FIRMWARE_IMAGE_INIT
            ? info.init_section_count
            : info.runtime_section_count;
    uint32_t chunks = 0;
    for (uint32_t i = 0; i < section_count; i++)
    {
        intel8265_firmware_section_t section;
        if (!intel8265_firmware_image_section_get(
                data, size, image, i, &section))
            return false;

        const uint64_t section_chunks =
            ((uint64_t)section.size + chunk_capacity - 1) /
            chunk_capacity;
        if (section_chunks > UINT32_MAX - chunks)
            return false;
        chunks += (uint32_t)section_chunks;
    }

    *payload_bytes = image == INTEL8265_FIRMWARE_IMAGE_INIT
        ? info.init_section_bytes
        : info.runtime_section_bytes;
    *chunk_count = chunks;
    return true;
}

bool intel8265_firmware_copy_tlv_chunk(
    const uint8_t *data,
    uint64_t size,
    uint32_t index,
    uint32_t offset,
    uint8_t *destination,
    uint32_t capacity,
    uint32_t *type,
    uint32_t *copied)
{
    if (!destination || !capacity || !type || !copied)
        return false;

    intel8265_firmware_tlv_t tlv;
    if (!intel8265_firmware_tlv_get(data, size, index, &tlv) ||
        offset >= tlv.length)
        return false;

    uint32_t amount = tlv.length - offset;
    if (amount > capacity)
        amount = capacity;
    for (uint32_t i = 0; i < amount; i++)
        destination[i] = tlv.data[offset + i];
    *type = tlv.type;
    *copied = amount;
    return true;
}

bool intel8265_firmware_copy_section_chunk(
    const uint8_t *data,
    uint64_t size,
    uint32_t index,
    uint32_t offset,
    uint8_t *destination,
    uint32_t capacity,
    uint32_t *device_address,
    uint32_t *copied)
{
    if (!destination || !capacity || !device_address || !copied)
        return false;

    intel8265_firmware_section_t section;
    if (!intel8265_firmware_section_get(
            data, size, index, &section) ||
        offset >= section.size)
        return false;

    uint32_t amount = section.size - offset;
    if (amount > capacity)
        amount = capacity;
    for (uint32_t i = 0; i < amount; i++)
        destination[i] = section.data[offset + i];
    *device_address = section.device_address + offset;
    *copied = amount;
    return true;
}

bool intel8265_firmware_copy_image_chunk(
    const uint8_t *data,
    uint64_t size,
    intel8265_firmware_image_t image,
    uint32_t section_index,
    uint32_t offset,
    uint8_t *destination,
    uint32_t capacity,
    uint32_t *device_address,
    uint32_t *copied)
{
    if (!destination || !capacity || !device_address || !copied ||
        (offset & 3))
        return false;

    intel8265_firmware_section_t section;
    if (!intel8265_firmware_image_section_get(
            data, size, image, section_index, &section) ||
        offset >= section.size)
        return false;

    uint32_t amount = section.size - offset;
    if (amount > capacity)
        amount = capacity;
    amount &= ~3U;
    if (!amount)
        return false;

    for (uint32_t i = 0; i < amount; i++)
        destination[i] = section.data[offset + i];
    *device_address = section.device_address + offset;
    *copied = amount;
    return true;
}

bool intel8265_firmware_copy_next_image_chunk(
    const uint8_t *data,
    uint64_t size,
    intel8265_firmware_image_t image,
    intel8265_firmware_cursor_t *cursor,
    uint8_t *destination,
    uint32_t capacity,
    uint32_t *device_address,
    uint32_t *copied,
    bool *complete)
{
    if (!cursor || !destination || !capacity ||
        !device_address || !copied || !complete)
        return false;

    intel8265_firmware_info_t info;
    if (!firmware_inspect_internal(data, size, &info, false) ||
        (image != INTEL8265_FIRMWARE_IMAGE_INIT &&
         image != INTEL8265_FIRMWARE_IMAGE_RUNTIME))
        return false;

    const uint32_t section_count =
        image == INTEL8265_FIRMWARE_IMAGE_INIT
            ? info.init_section_count
            : info.runtime_section_count;
    if (cursor->section_index > section_count)
        return false;
    if (cursor->section_index == section_count)
    {
        if (cursor->offset)
            return false;
        *device_address = 0;
        *copied = 0;
        *complete = true;
        return true;
    }

    intel8265_firmware_section_t section;
    if (!intel8265_firmware_image_section_get(
            data, size, image, cursor->section_index, &section) ||
        cursor->offset >= section.size || (cursor->offset & 3))
        return false;

    uint32_t address = 0;
    uint32_t amount = 0;
    if (!intel8265_firmware_copy_image_chunk(
            data, size, image, cursor->section_index, cursor->offset,
            destination, capacity, &address, &amount))
        return false;

    uint32_t next_section = cursor->section_index;
    uint32_t next_offset = cursor->offset + amount;
    if (next_offset == section.size)
    {
        next_section++;
        next_offset = 0;
    }

    cursor->section_index = next_section;
    cursor->offset = next_offset;
    *device_address = address;
    *copied = amount;
    *complete = next_section == section_count;
    return true;
}

bool intel8265_firmware_stage_tlv(
    uint32_t index,
    uint32_t offset,
    uint32_t *type,
    uint32_t *copied)
{
    if (!firmware_image || !firmware_image_size ||
        !resources.dma_virtual || !resources.dma_physical ||
        !type || !copied)
        return false;

    uint32_t staged = 0;
    if (!intel8265_firmware_copy_tlv_chunk(
            firmware_image,
            firmware_image_size,
            index,
            offset,
            resources.dma_virtual,
            (uint32_t)resources.dma_size,
            type,
            &staged))
        return false;

    for (uint32_t i = staged; i < resources.dma_size; i++)
        resources.dma_virtual[i] = 0;
    *copied = staged;
    return true;
}

bool intel8265_firmware_stage_section(
    uint32_t index,
    uint32_t offset,
    uint32_t *device_address,
    uint32_t *copied)
{
    if (!firmware_image || !firmware_image_size ||
        !resources.dma_virtual || !resources.dma_physical ||
        !resources.dma_size || resources.dma_size > UINT32_MAX ||
        !device_address || !copied)
        return false;

    uint32_t amount = 0;
    uint32_t address = 0;
    if (!intel8265_firmware_copy_section_chunk(
            firmware_image,
            firmware_image_size,
            index,
            offset,
            resources.dma_virtual,
            (uint32_t)resources.dma_size,
            &address,
            &amount))
        return false;
    for (uint32_t i = amount; i < resources.dma_size; i++)
        resources.dma_virtual[i] = 0;

    *device_address = address;
    *copied = amount;
    return true;
}

bool intel8265_firmware_stage_image_chunk(
    intel8265_firmware_image_t image,
    uint32_t section_index,
    uint32_t offset,
    uint32_t *device_address,
    uint32_t *copied)
{
    if (!firmware_image || !firmware_image_size ||
        !resources.dma_virtual || !resources.dma_physical ||
        resources.dma_size < sizeof(uint32_t) ||
        resources.dma_size > UINT32_MAX ||
        (resources.dma_size & 3) ||
        !device_address || !copied)
        return false;

    uint32_t amount = 0;
    uint32_t address = 0;
    if (!intel8265_firmware_copy_image_chunk(
            firmware_image,
            firmware_image_size,
            image,
            section_index,
            offset,
            resources.dma_virtual,
            (uint32_t)resources.dma_size,
            &address,
            &amount))
        return false;

    for (uint32_t i = amount; i < resources.dma_size; i++)
        resources.dma_virtual[i] = 0;
    *device_address = address;
    *copied = amount;
    return true;
}

bool intel8265_firmware_stage_next_image_chunk(
    intel8265_firmware_image_t image,
    intel8265_firmware_cursor_t *cursor,
    uint32_t *device_address,
    uint32_t *copied,
    bool *complete)
{
    if (!firmware_image || !firmware_image_size ||
        !resources.dma_virtual || !resources.dma_physical ||
        resources.dma_size > UINT32_MAX ||
        !device_address || !copied || !complete)
        return false;

    uint32_t amount = 0;
    uint32_t address = 0;
    if (!intel8265_firmware_copy_next_image_chunk(
            firmware_image,
            firmware_image_size,
            image,
            cursor,
            resources.dma_virtual,
            (uint32_t)resources.dma_size,
            &address,
            &amount,
            complete))
        return false;

    if (amount)
        for (uint32_t i = amount; i < resources.dma_size; i++)
            resources.dma_virtual[i] = 0;
    *device_address = address;
    *copied = amount;
    return true;
}

bool intel8265_matches_device(const pci_device_t *device)
{
    return device &&
        device->vendor_id == INTEL_VENDOR_ID &&
        device->device_id == INTEL_8265_8275_DEVICE_ID &&
        device->class_code == 0x02;
}

bool intel8265_probe(void)
{
    if (!pci_is_initialized())
        return false;

    device_count = 0;
    for (uint32_t i = 0; i < pci_device_count(); i++)
    {
        const pci_device_t *device = pci_device_get(i);
        if (!intel8265_matches_device(device))
            continue;
        if (device_count == PCI_DEVICE_LIMIT)
            break;
        devices[device_count++] = device;
    }

    if (!device_count)
        return true;

    serial_log_begin(SERIAL_LOG_INFO);
    serial_write("Detected Intel Wireless 8265 / 8275 (");
    serial_write_dec(device_count);
    serial_write(" device(s))");
    serial_log_end();
    return true;
}

bool intel8265_prepare_device(void)
{
    if (!device_count || resources.mmio || resources.dma_virtual)
        return resources.mmio && resources.dma_virtual;

    const pci_device_t *device = devices[0];
    pci_bar_info_t bar;
    if (!pci_get_bar_info(device, 0, &bar) || bar.is_io ||
        bar.size < INTEL8265_MMIO_MIN_SIZE ||
        bar.size > INTEL8265_MMIO_MAX_SIZE ||
        (bar.base & (INTEL8265_MMIO_MIN_SIZE - 1)))
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 BAR0 is not a valid bounded MMIO region");
        return false;
    }

    const uint64_t dma_physical = pmm_alloc_contiguous_pages(1);
    if (!dma_physical)
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Could not allocate contiguous Intel 8265 DMA memory");
        return false;
    }
    if (dma_physical >= INTEL8265_DMA_ADDRESS_LIMIT ||
        INTEL8265_DMA_PAGE_SIZE >
            INTEL8265_DMA_ADDRESS_LIMIT - dma_physical)
    {
        (void)pmm_free_page(dma_physical);
        serial_log(SERIAL_LOG_ERROR,
                   "Allocated Intel 8265 DMA page exceeds the 36-bit address limit");
        return false;
    }

    uint8_t *dma_virtual = paging_phys_to_virt(dma_physical);
    if (!dma_virtual)
    {
        (void)pmm_free_page(dma_physical);
        serial_log(SERIAL_LOG_ERROR,
                   "Could not map Intel 8265 DMA page into kernel memory");
        return false;
    }

    volatile uint8_t *mmio = ioremap(bar.base, bar.size);
    if (!mmio)
    {
        (void)pmm_free_page(dma_physical);
        serial_log(SERIAL_LOG_ERROR,
                   "Could not map Intel 8265 BAR0");
        return false;
    }

    for (uint64_t i = 0; i < INTEL8265_DMA_PAGE_SIZE; i++)
        dma_virtual[i] = 0;

    if (!pci_enable_device(device, true, true))
    {
        (void)pmm_free_page(dma_physical);
        serial_log(SERIAL_LOG_ERROR,
                   "Could not enable Intel 8265 PCI memory/bus mastering");
        return false;
    }

    resources = (intel8265_resources_t){
        .mmio_physical = bar.base,
        .mmio_size = bar.size,
        .mmio = mmio,
        .dma_physical = dma_physical,
        .dma_size = INTEL8265_DMA_PAGE_SIZE,
        .dma_virtual = dma_virtual
    };
    serial_log(SERIAL_LOG_INFO,
               "Intel 8265 PCI memory, BAR0 MMIO, and one DMA page prepared (device not started)");
    return true;
}

bool intel8265_resources_get(intel8265_resources_t *result)
{
    if (!result || !resources.mmio || !resources.dma_virtual)
        return false;
    *result = resources;
    return true;
}

bool intel8265_load_firmware_image(void)
{
    if (!device_count)
        return false;
    if (firmware_image)
        return true;

    vfs_file_t file;
    if (vfs_open(INTEL8265_FIRMWARE_PATH, &file) < 0)
    {
        serial_log_begin(SERIAL_LOG_WARN);
        serial_write("Intel 8265 firmware not found: ");
        serial_write(INTEL8265_FIRMWARE_PATH);
        serial_log_end();
        return false;
    }
    if (!file.size || file.size > INTEL8265_FIRMWARE_MAX_SIZE)
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 firmware has an invalid size");
        return false;
    }

    uint8_t *image = kmalloc(file.size);
    if (!image)
    {
        serial_log(SERIAL_LOG_ERROR,
                   "Could not allocate memory for Intel 8265 firmware");
        return false;
    }

    uint64_t loaded = 0;
    while (loaded < file.size)
    {
        const uint64_t remaining = file.size - loaded;
        const int64_t result = vfs_read(&file, image + loaded, remaining);
        if (result <= 0 || (uint64_t)result > remaining)
        {
            kfree(image);
            serial_log(SERIAL_LOG_ERROR,
                       "Could not read the complete Intel 8265 firmware");
            return false;
        }
        loaded += (uint64_t)result;
    }

    firmware_image = image;
    firmware_image_size = loaded;
    if (!intel8265_firmware_inspect(
            firmware_image, firmware_image_size, &firmware_info))
    {
        kfree(firmware_image);
        firmware_image = NULL;
        firmware_image_size = 0;
        serial_log(SERIAL_LOG_ERROR,
                   "Intel 8265 firmware has an invalid TLV container");
        return false;
    }

    serial_log_begin(SERIAL_LOG_INFO);
    serial_write("Intel 8265 firmware container validated (API ");
    serial_write_dec(firmware_info.api_version);
    serial_write(", ");
    serial_write_dec(firmware_info.tlv_count);
    serial_write(" TLVs, runtime/init sections ");
    serial_write_dec(firmware_info.runtime_section_count);
    serial_putc('/');
    serial_write_dec(firmware_info.init_section_count);
    serial_write(", runtime/init bytes ");
    serial_write_dec(firmware_info.runtime_section_bytes);
    serial_putc('/');
    serial_write_dec(firmware_info.init_section_bytes);
    serial_write(", build ");
    serial_write_dec(firmware_info.build);
    serial_write(", image bytes ");
    serial_write_dec(firmware_image_size);
    serial_write(", CRC32 0x");
    serial_write_hex(firmware_info.image_crc32);
    serial_write("); firmware upload is not implemented");
    serial_log_end();
    return true;
}

bool intel8265_firmware_image_get(const uint8_t **data, uint64_t *size)
{
    if (!data || !size || !firmware_image || !firmware_image_size)
        return false;
    *data = firmware_image;
    *size = firmware_image_size;
    return true;
}

bool intel8265_firmware_info_get(intel8265_firmware_info_t *info)
{
    if (!info || !firmware_image || !firmware_image_size)
        return false;
    *info = firmware_info;
    return true;
}

uint32_t intel8265_device_count(void)
{
    return device_count;
}

const pci_device_t *intel8265_device_get(uint32_t index)
{
    return index < device_count ? devices[index] : NULL;
}
