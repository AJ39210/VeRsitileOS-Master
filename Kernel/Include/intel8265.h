#ifndef INTEL8265_H
#define INTEL8265_H

#include <stdbool.h>
#include <stdint.h>

#include "pci.h"

#define INTEL8265_FIRMWARE_PATH "/firmware/iwlwifi-8265-36.ucode"
#define INTEL8265_FIRMWARE_MAX_SIZE (4U * 1024U * 1024U)
#define INTEL8265_DMA_PAGE_SIZE 4096U

enum
{
    INTEL8265_FIRMWARE_API_VERSION = 36,
    INTEL8265_TLV_SEC_RT = 19,
    INTEL8265_TLV_SEC_INIT = 20,
    INTEL8265_TLV_SEC_WOWLAN = 21
};

typedef enum
{
    INTEL8265_FIRMWARE_IMAGE_INIT = INTEL8265_TLV_SEC_INIT,
    INTEL8265_FIRMWARE_IMAGE_RUNTIME = INTEL8265_TLV_SEC_RT
} intel8265_firmware_image_t;

typedef struct
{
    uint32_t api_version;
    uint32_t build;
    uint32_t tlv_count;
    uint32_t runtime_section_count;
    uint32_t init_section_count;
    uint32_t wowlan_section_count;
    uint64_t runtime_section_bytes;
    uint64_t init_section_bytes;
    uint64_t wowlan_section_bytes;
    uint32_t image_crc32;
} intel8265_firmware_info_t;

typedef struct
{
    uint32_t type;
    uint32_t length;
    const uint8_t *data;
} intel8265_firmware_tlv_t;

typedef struct
{
    uint32_t type;
    uint32_t device_address;
    uint32_t size;
    const uint8_t *data;
} intel8265_firmware_section_t;

typedef struct
{
    uint32_t section_index;
    uint32_t offset;
} intel8265_firmware_cursor_t;

typedef struct
{
    uint64_t mmio_physical;
    uint64_t mmio_size;
    volatile uint8_t *mmio;
    uint64_t dma_physical;
    uint64_t dma_size;
    uint8_t *dma_virtual;
} intel8265_resources_t;

bool intel8265_matches_device(const pci_device_t *device);
bool intel8265_probe(void);
bool intel8265_prepare_device(void);
bool intel8265_resources_get(intel8265_resources_t *resources);
bool intel8265_load_firmware_image(void);
bool intel8265_firmware_image_get(const uint8_t **data, uint64_t *size);
bool intel8265_firmware_info_get(intel8265_firmware_info_t *info);
bool intel8265_firmware_crc32(
    const uint8_t *data,
    uint64_t size,
    uint32_t *checksum);
bool intel8265_firmware_inspect(
    const uint8_t *data,
    uint64_t size,
    intel8265_firmware_info_t *info);
bool intel8265_firmware_tlv_get(
    const uint8_t *data,
    uint64_t size,
    uint32_t index,
    intel8265_firmware_tlv_t *tlv);
bool intel8265_firmware_copy_tlv_chunk(
    const uint8_t *data,
    uint64_t size,
    uint32_t index,
    uint32_t offset,
    uint8_t *destination,
    uint32_t capacity,
    uint32_t *type,
    uint32_t *copied);
bool intel8265_firmware_section_get(
    const uint8_t *data,
    uint64_t size,
    uint32_t index,
    intel8265_firmware_section_t *section);
bool intel8265_firmware_image_section_get(
    const uint8_t *data,
    uint64_t size,
    intel8265_firmware_image_t image,
    uint32_t section_index,
    intel8265_firmware_section_t *section);
bool intel8265_firmware_image_plan(
    const uint8_t *data,
    uint64_t size,
    intel8265_firmware_image_t image,
    uint32_t chunk_capacity,
    uint64_t *payload_bytes,
    uint32_t *chunk_count);
bool intel8265_firmware_copy_section_chunk(
    const uint8_t *data,
    uint64_t size,
    uint32_t index,
    uint32_t offset,
    uint8_t *destination,
    uint32_t capacity,
    uint32_t *device_address,
    uint32_t *copied);
bool intel8265_firmware_copy_image_chunk(
    const uint8_t *data,
    uint64_t size,
    intel8265_firmware_image_t image,
    uint32_t section_index,
    uint32_t offset,
    uint8_t *destination,
    uint32_t capacity,
    uint32_t *device_address,
    uint32_t *copied);
bool intel8265_firmware_copy_next_image_chunk(
    const uint8_t *data,
    uint64_t size,
    intel8265_firmware_image_t image,
    intel8265_firmware_cursor_t *cursor,
    uint8_t *destination,
    uint32_t capacity,
    uint32_t *device_address,
    uint32_t *copied,
    bool *complete);
bool intel8265_firmware_stage_tlv(
    uint32_t index,
    uint32_t offset,
    uint32_t *type,
    uint32_t *copied);
bool intel8265_firmware_stage_section(
    uint32_t index,
    uint32_t offset,
    uint32_t *device_address,
    uint32_t *copied);
bool intel8265_firmware_stage_image_chunk(
    intel8265_firmware_image_t image,
    uint32_t section_index,
    uint32_t offset,
    uint32_t *device_address,
    uint32_t *copied);
bool intel8265_firmware_stage_next_image_chunk(
    intel8265_firmware_image_t image,
    intel8265_firmware_cursor_t *cursor,
    uint32_t *device_address,
    uint32_t *copied,
    bool *complete);
uint32_t intel8265_device_count(void);
const pci_device_t *intel8265_device_get(uint32_t index);

#endif
