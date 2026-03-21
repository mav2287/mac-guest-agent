#ifndef VIRTIO_TYPES_H
#define VIRTIO_TYPES_H

#include <stdint.h>

/*
 * VirtIO PCI device identification
 * Reference: VirtIO Spec v1.2, Section 4.1
 */
#define VIRTIO_PCI_VENDOR_ID        0x1AF4

/* Legacy/transitional console device ID */
#define VIRTIO_PCI_DEVICE_CONSOLE_LEGACY    0x1003
/* Modern console device ID */
#define VIRTIO_PCI_DEVICE_CONSOLE_MODERN    0x1043

/*
 * VirtIO PCI Legacy Register Offsets (BAR0)
 * Reference: VirtIO Spec v1.2, Section 4.1.4.8 (Legacy Interface)
 */
#define VIRTIO_PCI_HOST_FEATURES        0x00    /* 32-bit, R   */
#define VIRTIO_PCI_GUEST_FEATURES       0x04    /* 32-bit, R/W */
#define VIRTIO_PCI_QUEUE_PFN            0x08    /* 32-bit, R/W */
#define VIRTIO_PCI_QUEUE_NUM            0x0C    /* 16-bit, R   */
#define VIRTIO_PCI_QUEUE_SEL            0x0E    /* 16-bit, R/W */
#define VIRTIO_PCI_QUEUE_NOTIFY         0x10    /* 16-bit, R/W */
#define VIRTIO_PCI_STATUS               0x12    /* 8-bit,  R/W */
#define VIRTIO_PCI_ISR                  0x13    /* 8-bit,  R   */
/* Device-specific config starts at offset 20 (0x14) for legacy */
#define VIRTIO_PCI_CONFIG_OFF           0x14

/*
 * VirtIO Device Status Bits
 * Reference: VirtIO Spec v1.2, Section 2.1
 */
#define VIRTIO_STATUS_ACKNOWLEDGE       0x01
#define VIRTIO_STATUS_DRIVER            0x02
#define VIRTIO_STATUS_DRIVER_OK         0x04
#define VIRTIO_STATUS_FEATURES_OK       0x08
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 0x40
#define VIRTIO_STATUS_FAILED            0x80

/*
 * VirtIO Console Feature Bits
 * Reference: VirtIO Spec v1.2, Section 5.3.2
 */
#define VIRTIO_CONSOLE_F_SIZE           (1 << 0)
#define VIRTIO_CONSOLE_F_MULTIPORT      (1 << 1)
#define VIRTIO_CONSOLE_F_EMERG_WRITE    (1 << 2)

/*
 * VirtIO Console Configuration Space (at VIRTIO_PCI_CONFIG_OFF)
 * Reference: VirtIO Spec v1.2, Section 5.3.4
 */
struct virtio_console_config {
    uint16_t cols;
    uint16_t rows;
    uint32_t max_nr_ports;
    uint32_t emerg_wr;
} __attribute__((packed));

/*
 * VirtIO Console Control Message
 * Reference: VirtIO Spec v1.2, Section 5.3.6.2
 */
struct virtio_console_control {
    uint32_t id;        /* Port number (or VIRTIO_CONSOLE_BAD_ID) */
    uint16_t event;     /* Control event type */
    uint16_t value;     /* Event-specific value */
} __attribute__((packed));

#define VIRTIO_CONSOLE_BAD_ID           0xFFFFFFFF

/* Control event types */
#define VIRTIO_CONSOLE_DEVICE_READY     0
#define VIRTIO_CONSOLE_PORT_ADD         1
#define VIRTIO_CONSOLE_PORT_REMOVE      2
#define VIRTIO_CONSOLE_PORT_READY       3
#define VIRTIO_CONSOLE_CONSOLE_PORT     4
#define VIRTIO_CONSOLE_RESIZE           5
#define VIRTIO_CONSOLE_PORT_OPEN        6
#define VIRTIO_CONSOLE_PORT_NAME        7

/*
 * VirtIO ISR Status Bits
 */
#define VIRTIO_ISR_QUEUE                0x01
#define VIRTIO_ISR_CONFIG               0x02

/*
 * Virtqueue Descriptor Flags
 * Reference: VirtIO Spec v1.2, Section 2.7.5
 */
#define VIRTQ_DESC_F_NEXT               1   /* Descriptor chains to next */
#define VIRTQ_DESC_F_WRITE              2   /* Device writes (vs reads) */
#define VIRTQ_DESC_F_INDIRECT           4   /* Buffer contains descriptor list */

/*
 * Virtqueue Descriptor
 */
struct virtq_desc {
    uint64_t addr;      /* Physical address of buffer */
    uint32_t len;       /* Length of buffer */
    uint16_t flags;     /* VIRTQ_DESC_F_* */
    uint16_t next;      /* Next descriptor index (if F_NEXT) */
} __attribute__((packed));

/*
 * Virtqueue Available Ring
 */
struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];    /* Variable length: ring[queue_size] */
} __attribute__((packed));

/*
 * Virtqueue Used Ring Element
 */
struct virtq_used_elem {
    uint32_t id;        /* Descriptor head index */
    uint32_t len;       /* Bytes written by device */
} __attribute__((packed));

/*
 * Virtqueue Used Ring
 */
struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];  /* Variable length */
} __attribute__((packed));

/*
 * Page size for virtqueue alignment
 */
#define VIRTIO_PAGE_SIZE    4096

/*
 * Calculate the total size of a virtqueue's ring structures
 * Reference: VirtIO Spec v1.2, Section 2.7
 */
static inline uint32_t virtq_ring_size(uint16_t queue_size)
{
    /* Descriptor table + available ring (with padding to page boundary) */
    uint32_t desc_avail = sizeof(struct virtq_desc) * queue_size
                        + sizeof(uint16_t) * (3 + queue_size);
    /* Align to page */
    desc_avail = (desc_avail + VIRTIO_PAGE_SIZE - 1) & ~(VIRTIO_PAGE_SIZE - 1);

    /* Used ring */
    uint32_t used = sizeof(uint16_t) * 3
                  + sizeof(struct virtq_used_elem) * queue_size;
    used = (used + VIRTIO_PAGE_SIZE - 1) & ~(VIRTIO_PAGE_SIZE - 1);

    return desc_avail + used;
}

/* Buffer sizes matching Linux driver */
#define VIRTIO_SERIAL_BUF_SIZE      4096    /* PAGE_SIZE */
#define VIRTIO_SERIAL_MAX_WRITE     32768   /* 32KB max write */

#endif /* VIRTIO_TYPES_H */
