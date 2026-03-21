#ifndef VIRTIO_PCI_DEVICE_H
#define VIRTIO_PCI_DEVICE_H

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "VirtIOTypes.h"
#include "VirtQueue.h"

/*
 * VirtIOPCIDevice — VirtIO PCI transport layer for macOS IOKit.
 *
 * Handles BAR mapping, feature negotiation, device status management,
 * and virtqueue initialization for VirtIO legacy/transitional devices.
 *
 * Reference: VirtIO Spec v1.2, Section 4.1 (Virtio Over PCI Bus)
 */

#define MAX_VIRTQUEUES 16

class VirtIOPCIDevice : public IOService {
    OSDeclareDefaultStructors(VirtIOPCIDevice)

public:
    virtual bool        start(IOService *provider) override;
    virtual void        stop(IOService *provider) override;
    virtual void        free() override;

protected:
    /* PCI transport operations */
    bool                mapBAR0();
    void                unmapBAR0();

    /* Device status management */
    void                resetDevice();
    void                setStatus(uint8_t status);
    uint8_t             getStatus();
    void                addStatus(uint8_t status);

    /* Feature negotiation */
    uint32_t            readDeviceFeatures();
    void                writeDriverFeatures(uint32_t features);

    /* Device-specific config access (at offset 0x14+) */
    uint8_t             readConfig8(uint32_t offset);
    uint16_t            readConfig16(uint32_t offset);
    uint32_t            readConfig32(uint32_t offset);
    void                writeConfig8(uint32_t offset, uint8_t value);
    void                writeConfig16(uint32_t offset, uint16_t value);
    void                writeConfig32(uint32_t offset, uint32_t value);

    /* Virtqueue setup */
    bool                setupQueue(uint16_t index, VirtQueue *queue);
    void                notifyQueue(uint16_t index);

    /* ISR */
    uint8_t             readISR();

    /* Provider */
    IOPCIDevice         *pciDevice;
    IOMemoryMap         *bar0Map;
    volatile uint8_t    *bar0Base;
};

#endif /* VIRTIO_PCI_DEVICE_H */
