#include "VirtQueue.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/OSAtomic.h>
#include <kern/debug.h>

/* OSMemoryBarrier may not be available in all SDK versions.
   Use OSSynchronizeIO which is the IOKit equivalent. */
#ifndef OSMemoryBarrier
#define OSMemoryBarrier() OSSynchronizeIO()
#endif

bool VirtQueue::init(uint16_t size)
{
    if (size == 0 || (size & (size - 1)) != 0) {
        IOLog("VirtQueue: size must be power of 2, got %u\n", size);
        return false;
    }

    queueSize = size;
    freeCount = size;
    freeHead = 0;
    lastUsedIdx = 0;

    /* Allocate contiguous, page-aligned ring memory */
    uint32_t ringSize = virtq_ring_size(size);

    ringMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        ringSize,
        0xFFFFF000ULL   /* Page-aligned physical mask */
    );

    if (!ringMem) {
        IOLog("VirtQueue: failed to allocate %u bytes for ring\n", ringSize);
        return false;
    }

    ringMem->prepare();
    void *ringBase = ringMem->getBytesNoCopy();
    bzero(ringBase, ringSize);

    /* Calculate offsets into the ring memory */
    desc = (struct virtq_desc *)ringBase;

    uint32_t availOffset = sizeof(struct virtq_desc) * size;
    avail = (struct virtq_avail *)((uint8_t *)ringBase + availOffset);

    /* Used ring starts at next page boundary after desc + avail */
    uint32_t usedOffset = sizeof(struct virtq_desc) * size
                        + sizeof(uint16_t) * (3 + size);
    usedOffset = (usedOffset + VIRTIO_PAGE_SIZE - 1) & ~(VIRTIO_PAGE_SIZE - 1);
    used = (struct virtq_used *)((uint8_t *)ringBase + usedOffset);

    /* Initialize free descriptor chain: 0→1→2→...→(size-1) */
    for (uint16_t i = 0; i < size - 1; i++) {
        desc[i].next = i + 1;
    }
    desc[size - 1].next = 0xFFFF; /* End of chain sentinel */

    /* Allocate buffer tracking array */
    buffers = (void **)IOMalloc(sizeof(void *) * size);
    if (!buffers) {
        ringMem->complete();
        ringMem->release();
        ringMem = NULL;
        return false;
    }
    bzero(buffers, sizeof(void *) * size);

    return true;
}

void VirtQueue::free()
{
    if (buffers) {
        IOFree(buffers, sizeof(void *) * queueSize);
        buffers = NULL;
    }
    if (ringMem) {
        ringMem->complete();
        ringMem->release();
        ringMem = NULL;
    }
    desc = NULL;
    avail = NULL;
    used = NULL;
}

uint64_t VirtQueue::getPhysicalAddress() const
{
    if (!ringMem) return 0;
    return ringMem->getPhysicalAddress();
}

int VirtQueue::allocDesc()
{
    if (freeCount == 0)
        return -1;

    uint16_t idx = freeHead;
    freeHead = desc[idx].next;
    freeCount--;
    return idx;
}

void VirtQueue::freeDesc(uint16_t idx)
{
    if (idx >= queueSize) return;

    desc[idx].addr = 0;
    desc[idx].len = 0;
    desc[idx].flags = 0;
    desc[idx].next = freeHead;
    freeHead = idx;
    freeCount++;
    buffers[idx] = NULL;
}

int VirtQueue::addInputBuffer(void *buf, uint32_t len, uint64_t physAddr)
{
    int idx = allocDesc();
    if (idx < 0) return -1;

    desc[idx].addr = physAddr;
    desc[idx].len = len;
    desc[idx].flags = VIRTQ_DESC_F_WRITE;  /* Device-writable */
    desc[idx].next = 0;
    buffers[idx] = buf;

    /* Add to available ring */
    uint16_t availIdx = avail->idx;
    avail->ring[availIdx % queueSize] = (uint16_t)idx;

    /* Memory barrier: ensure descriptor is visible before updating index */
    OSMemoryBarrier();

    avail->idx = availIdx + 1;

    return idx;
}

int VirtQueue::addOutputBuffer(const void *buf, uint32_t len, uint64_t physAddr)
{
    int idx = allocDesc();
    if (idx < 0) return -1;

    desc[idx].addr = physAddr;
    desc[idx].len = len;
    desc[idx].flags = 0;   /* Device-readable */
    desc[idx].next = 0;
    buffers[idx] = (void *)buf;

    /* Add to available ring */
    uint16_t availIdx = avail->idx;
    avail->ring[availIdx % queueSize] = (uint16_t)idx;

    OSMemoryBarrier();

    avail->idx = availIdx + 1;

    return idx;
}

int VirtQueue::getUsedBuffer(uint32_t *len)
{
    if (!hasUsedBuffers())
        return -1;

    /* Memory barrier: ensure we read fresh used ring data */
    OSMemoryBarrier();

    struct virtq_used_elem *elem = &used->ring[lastUsedIdx % queueSize];
    uint16_t descIdx = (uint16_t)elem->id;
    if (len)
        *len = elem->len;

    lastUsedIdx++;

    /* Free the descriptor */
    void *buf = buffers[descIdx];
    freeDesc(descIdx);

    /* Return descriptor index; caller can retrieve buffer via the return value.
       We store the buffer pointer at the desc index for caller retrieval. */
    buffers[descIdx] = buf; /* Restore temporarily for caller */
    return descIdx;
}

bool VirtQueue::hasUsedBuffers() const
{
    OSMemoryBarrier();
    return lastUsedIdx != used->idx;
}

bool VirtQueue::needsNotification() const
{
    /* In legacy mode, always notify unless VIRTQ_AVAIL_F_NO_INTERRUPT is set */
    return !(used->flags & 1);
}
