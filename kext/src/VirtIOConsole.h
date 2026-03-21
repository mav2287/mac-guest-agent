#ifndef VIRTIO_CONSOLE_H
#define VIRTIO_CONSOLE_H

#include "VirtIOPCIDevice.h"
#include "VirtQueue.h"
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>

/*
 * VirtIOConsole — Main driver for VirtIO console/serial PCI device.
 *
 * Handles device initialization, multiport protocol, interrupt handling,
 * and port lifecycle. Creates VirtIOSerialPort children for each discovered port.
 *
 * Matches the Linux kernel's virtio_console.c driver behavior.
 */

#define MAX_PORTS 16

/* Per-port state */
struct VirtIOPort {
    bool            active;
    bool            host_connected;
    char            name[128];
    VirtQueue       receiveq;
    VirtQueue       transmitq;
    uint16_t        receiveq_idx;
    uint16_t        transmitq_idx;
    IOService       *serialPort;        /* Child VirtIOSerialPort nub */

    /* Receive buffer pool */
    void            *recv_bufs[32];
    uint64_t        recv_phys[32];
    int             recv_buf_count;
};

class VirtIOConsole : public VirtIOPCIDevice {
    OSDeclareDefaultStructors(VirtIOConsole)

public:
    virtual IOService   *probe(IOService *provider, SInt32 *score) override;
    virtual bool         start(IOService *provider) override;
    virtual void         stop(IOService *provider) override;
    virtual void         free() override;

    /* Called by VirtIOSerialPort to send data */
    int                  writePort(uint32_t portId, const void *data, uint32_t len);

    /* Called by VirtIOSerialPort to read data */
    int                  readPort(uint32_t portId, void *buf, uint32_t len, uint32_t *bytesRead);

    /* Called by VirtIOSerialPort on open/close */
    void                 portOpened(uint32_t portId);
    void                 portClosed(uint32_t portId);

private:
    /* Device initialization */
    bool                negotiateFeatures();
    bool                initQueues();
    bool                startMultiport();

    /* Control message handling */
    void                sendControlMsg(uint32_t id, uint16_t event, uint16_t value);
    void                processControlMessages();
    void                handleControlMessage(struct virtio_console_control *ctrl, const void *extra, uint32_t extraLen);

    /* Port management */
    void                addPort(uint32_t id);
    void                removePort(uint32_t id);
    void                setPortName(uint32_t id, const char *name);
    void                setPortOpen(uint32_t id, bool open);
    bool                createSerialPortNub(uint32_t id);

    /* Pre-fill receive queues */
    void                fillReceiveQueue(struct VirtIOPort *port);
    void                fillControlReceiveQueue();

    /* Interrupt handling */
    static bool         interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void         interruptAction(OSObject *owner, IOFilterInterruptEventSource *src, int count);
    void                handleInterrupt();

    /* Timer for polling (fallback if interrupts don't work) */
    static void         timerFired(OSObject *owner, IOTimerEventSource *sender);

    /* State */
    uint32_t            deviceFeatures;
    uint32_t            driverFeatures;
    bool                multiport;
    uint32_t            maxPorts;

    struct VirtIOPort   ports[MAX_PORTS];

    /* Control queues (multiport only) */
    VirtQueue           controlRecvQ;
    VirtQueue           controlSendQ;
    uint16_t            controlRecvQIdx;
    uint16_t            controlSendQIdx;

    /* Control receive buffers */
    void                *ctrl_recv_bufs[16];
    uint64_t            ctrl_recv_phys[16];
    int                 ctrl_recv_count;

    /* Control send buffer (reusable) */
    IOBufferMemoryDescriptor *ctrlSendBuf;

    IOFilterInterruptEventSource *intSource;
    IOTimerEventSource  *timerSource;
    IOWorkLoop          *workLoop;
};

#endif /* VIRTIO_CONSOLE_H */
