#include <assert.h>
#include <stdint.h>

#include "ar5007eg.h"

int main(void)
{
    assert(!ar5007eg_matches_device(NULL));

    pci_device_t device = {
        .vendor_id = AR5007EG_VENDOR_ID,
        .device_id = AR5007EG_DEVICE_ID,
        .class_code = 0x02
    };
    assert(ar5007eg_matches_device(&device));

    device.device_id++;
    assert(!ar5007eg_matches_device(&device));
    device.device_id = AR5007EG_DEVICE_ID;
    device.vendor_id++;
    assert(!ar5007eg_matches_device(&device));
    device.vendor_id = AR5007EG_VENDOR_ID;
    device.class_code = 0x03;
    assert(!ar5007eg_matches_device(&device));
    return 0;
}
