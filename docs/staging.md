XX - Added ( fully tested and no bugs)
X! - Critical work needed the driver or / and multiples files needs to be heavy updated and etc.
X  - Added ( Bugs may appear or its just stubs like just proof of concepts and etc)
?  - Needs to be updated 
X? - Updated 
!X - Needs to be Decomisioned / removed 
!Y - Removed
?? - Other stuff ( needs to be specifed )

Need to add / validate :
            RTL8102E / RTL8103E driver implementation is present as a shared
            descriptor-ring module; physical-hardware validation remains.
          
            X! Qualcomm Atheros AR5007EG/AR2425 PCI discovery and bounded BAR0
            mapping are implemented. Radio initialization, DMA, 802.11 TX/RX,
            scanning, WPA2-Personal, network-stack integration, and a native
            iwctl-style control command remain to be implemented and tested.

            NVMe / AHCI (SATA)

Need to update:
            Intel 8265 PCI/MMIO/DMA setup, API-36 validation, init/runtime
            firmware section staging, sequential image cursors, payload size
            reporting, aligned chunk planning, and diagnostic CRC-32
            fingerprinting are added. The firmware blob is read into kernel
            memory and validated, but no firmware bytes are uploaded to the
            adapter; hardware startup, interrupts/events, and Wi-Fi scanning
            remain X!
            
            AMD/NVIDIA/INTEL drivers ( will always need to update them and add more stuff including drms ) ?? 


Need to fix:
            