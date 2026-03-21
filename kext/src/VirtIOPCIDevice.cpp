#include "VirtIOPCIDevice.h"
#include <IOKit/IOLib.h>

#define super IOService
OSDefineMetaClassAndStructors(VirtIOPCIDevice, IOService)

bool VirtIOPCIDevice::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        IOLog("VirtIOPCI: provider is not IOPCIDevice\n");
        return false;
    }

    pciDevice->retain();

    /* Enable PCI bus mastering and memory space */
    pciDevice->setBusMasterEnable(true);
    pciDevice->setMemoryEnable(true);

    if (!mapBAR0()) {
        pciDevice->release();
        pciDevice = NULL;
        return false;
    }

    return true;
}

void VirtIOPCIDevice::stop(IOService *provider)
{
    resetDevice();
    unmapBAR0();

    if (pciDevice) {
        pciDevice->release();
        pciDevice = NULL;
    }

    super::stop(provider);
}

void VirtIOPCIDevice::free()
{
    super::free();
}

/* ---- BAR Mapping ---- */

bool VirtIOPCIDevice::mapBAR0()
{
    IODeviceMemory *devMem = pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!devMem) {
        IOLog("VirtIOPCI: no memory at BAR0\n");
        return false;
    }

    bar0Map = devMem->map();
    if (!bar0Map) {
        IOLog("VirtIOPCI: failed to map BAR0\n");
        return false;
    }

    bar0Base = (volatile uint8_t *)bar0Map->getVirtualAddress();
    IOLog("VirtIOPCI: BAR0 mapped at %p, length %llu\n",
          (void *)bar0Base, bar0Map->getLength());

    return true;
}

void VirtIOPCIDevice::unmapBAR0()
{
    if (bar0Map) {
        bar0Map->release();
        bar0Map = NULL;
    }
    bar0Base = NULL;
}

/* ---- Device Status ---- */

void VirtIOPCIDevice::resetDevice()
{
    if (!bar0Base) return;
    /* Writing 0 to status register resets the device */
    *(volatile uint8_t *)(bar0Base + VIRTIO_PCI_STATUS) = 0;
    /* Read back to ensure the write completes */
    (void)*(volatile uint8_t *)(bar0Base + VIRTIO_PCI_STATUS);
    IOLog("VirtIOPCI: device reset\n");
}

void VirtIOPCIDevice::setStatus(uint8_t status)
{
    *(volatile uint8_t *)(bar0Base + VIRTIO_PCI_STATUS) = status;
}

uint8_t VirtIOPCIDevice::getStatus()
{
    return *(volatile uint8_t *)(bar0Base + VIRTIO_PCI_STATUS);
}

void VirtIOPCIDevice::addStatus(uint8_t status)
{
    uint8_t cur = getStatus();
    setStatus(cur | status);
}

/* ---- Feature Negotiation ---- */

uint32_t VirtIOPCIDevice::readDeviceFeatures()
{
    return *(volatile uint32_t *)(bar0Base + VIRTIO_PCI_HOST_FEATURES);
}

void VirtIOPCIDevice::writeDriverFeatures(uint32_t features)
{
    *(volatile uint32_t *)(bar0Base + VIRTIO_PCI_GUEST_FEATURES) = features;
}

/* ---- Device-Specific Config ---- */

uint8_t VirtIOPCIDevice::readConfig8(uint32_t offset)
{
    return *(volatile uint8_t *)(bar0Base + VIRTIO_PCI_CONFIG_OFF + offset);
}

uint16_t VirtIOPCIDevice::readConfig16(uint32_t offset)
{
    return *(volatile uint16_t *)(bar0Base + VIRTIO_PCI_CONFIG_OFF + offset);
}

uint32_t VirtIOPCIDevice::readConfig32(uint32_t offset)
{
    return *(volatile uint32_t *)(bar0Base + VIRTIO_PCI_CONFIG_OFF + offset);
}

void VirtIOPCIDevice::writeConfig8(uint32_t offset, uint8_t value)
{
    *(volatile uint8_t *)(bar0Base + VIRTIO_PCI_CONFIG_OFF + offset) = value;
}

void VirtIOPCIDevice::writeConfig16(uint32_t offset, uint16_t value)
{
    *(volatile uint16_t *)(bar0Base + VIRTIO_PCI_CONFIG_OFF + offset) = value;
}

void VirtIOPCIDevice::writeConfig32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(bar0Base + VIRTIO_PCI_CONFIG_OFF + offset) = value;
}

/* ---- Virtqueue Setup ---- */

bool VirtIOPCIDevice::setupQueue(uint16_t index, VirtQueue *queue)
{
    if (!bar0Base || !queue) return false;

    /* Select the queue */
    *(volatile uint16_t *)(bar0Base + VIRTIO_PCI_QUEUE_SEL) = index;

    /* Read queue size from device */
    uint16_t size = *(volatile uint16_t *)(bar0Base + VIRTIO_PCI_QUEUE_NUM);
    if (size == 0) {
        IOLog("VirtIOPCI: queue %u has size 0\n", index);
        return false;
    }

    IOLog("VirtIOPCI: queue %u size=%u\n", index, size);

    /* Initialize the virtqueue */
    if (!queue->init(size))
        return false;

    /* Write the physical page frame number to the device */
    uint64_t physAddr = queue->getPhysicalAddress();
    uint32_t pfn = (uint32_t)(physAddr / VIRTIO_PAGE_SIZE);
    *(volatile uint32_t *)(bar0Base + VIRTIO_PCI_QUEUE_PFN) = pfn;

    IOLog("VirtIOPCI: queue %u initialized, PFN=0x%x\n", index, pfn);
    return true;
}

void VirtIOPCIDevice::notifyQueue(uint16_t index)
{
    if (!bar0Base) return;
    *(volatile uint16_t *)(bar0Base + VIRTIO_PCI_QUEUE_NOTIFY) = index;
}

/* ---- ISR ---- */

uint8_t VirtIOPCIDevice::readISR()
{
    return *(volatile uint8_t *)(bar0Base + VIRTIO_PCI_ISR);
}
