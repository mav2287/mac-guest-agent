#ifndef VIRTIO_SERIAL_PORT_H
#define VIRTIO_SERIAL_PORT_H

#include <IOKit/IOService.h>

class VirtIOConsole;

/*
 * VirtIOSerialPort — Exposes a VirtIO serial port as /dev/cu.* and /dev/tty.*
 *
 * Creates a BSD character device node so userspace programs (like the guest agent)
 * can open, read, and write the port using standard file I/O.
 *
 * Uses the devfs_make_node() / cdevsw API to create the device directly,
 * bypassing IOSerialFamily (which requires headers not in the public SDK).
 */

class VirtIOSerialPort : public IOService {
    OSDeclareDefaultStructors(VirtIOSerialPort)

public:
    bool            init(uint32_t portId, const char *portName, VirtIOConsole *console);
    virtual bool    start(IOService *provider) override;
    virtual void    stop(IOService *provider) override;
    virtual void    free() override;

    /* Character device operations (called from cdevsw handlers) */
    int             devOpen(int flags);
    int             devClose();
    int             devRead(void *buf, uint32_t len, uint32_t *bytesRead);
    int             devWrite(const void *buf, uint32_t len);

    uint32_t        getPortId() const { return portId; }

private:
    bool            createCharDevice();
    void            destroyCharDevice();

    uint32_t        portId;
    char            portName[128];
    VirtIOConsole   *console;

    int             majorNum;
    void            *devNode_cu;    /* devfs node for /dev/cu.* */
    void            *devNode_tty;   /* devfs node for /dev/tty.* */
    bool            isOpen;
};

/* Global port lookup for cdevsw callbacks */
VirtIOSerialPort *VirtIOSerialPort_findByMinor(int minor);

#endif /* VIRTIO_SERIAL_PORT_H */
