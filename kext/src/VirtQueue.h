#ifndef VIRTQUEUE_H
#define VIRTQUEUE_H

#include "VirtIOTypes.h"

#ifdef KERNEL
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#endif

/*
 * VirtQueue — split-ring virtqueue implementation for macOS IOKit.
 *
 * Manages the descriptor table, available ring, and used ring for a single
 * VirtIO virtqueue. Matches the Linux kernel's split-ring implementation.
 *
 * Memory layout (contiguous, page-aligned):
 *   [descriptor table] [available ring] [padding to page] [used ring]
 */

class VirtQueue {
public:
    /* Allocate and initialize a virtqueue with the given size (must be power of 2) */
    bool init(uint16_t size);

    /* Free all allocated resources */
    void free();

    /* Get the physical address of the ring memory (for writing to PCI config) */
    uint64_t getPhysicalAddress() const;

    /*
     * Add an input buffer (device-writable) to the available ring.
     * Returns descriptor index, or -1 on failure.
     */
    int addInputBuffer(void *buf, uint32_t len, uint64_t physAddr);

    /*
     * Add an output buffer (device-readable) to the available ring.
     * Returns descriptor index, or -1 on failure.
     */
    int addOutputBuffer(const void *buf, uint32_t len, uint64_t physAddr);

    /*
     * Get a completed buffer from the used ring.
     * Returns the descriptor index and sets *len to bytes written by device.
     * Returns -1 if no completed buffers.
     */
    int getUsedBuffer(uint32_t *len);

    /* Check if there are completed buffers in the used ring */
    bool hasUsedBuffers() const;

    /* Get the queue size */
    uint16_t getSize() const { return queueSize; }

    /* Get number of free descriptors */
    uint16_t getFreeCount() const { return freeCount; }

    /* Notify the device that new buffers are available.
     * Caller must write queueIndex to the notify register. */
    bool needsNotification() const;

private:
    int allocDesc();
    void freeDesc(uint16_t idx);

    uint16_t                queueSize;
    uint16_t                freeCount;
    uint16_t                freeHead;       /* Head of free descriptor list */
    uint16_t                lastUsedIdx;    /* Last used ring index we processed */

    /* Pointers into the ring memory */
    struct virtq_desc       *desc;
    struct virtq_avail      *avail;
    struct virtq_used       *used;

    /* Track buffer pointers by descriptor index so we can return them */
    void                    **buffers;

#ifdef KERNEL
    IOBufferMemoryDescriptor *ringMem;
#else
    void *ringMem;
#endif
};

#endif /* VIRTQUEUE_H */
