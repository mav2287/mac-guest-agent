#include "VirtIOSerialPort.h"
#include "VirtIOConsole.h"
#include <IOKit/IOLib.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/errno.h>
#include <miscfs/devfs/devfs.h>

#define super IOService
OSDefineMetaClassAndStructors(VirtIOSerialPort, IOService)

/* ---- Global Port Registry ---- */

#define MAX_SERIAL_PORTS 16
static VirtIOSerialPort *registeredPorts[MAX_SERIAL_PORTS];
static int registeredMajor = -1;

VirtIOSerialPort *VirtIOSerialPort_findByMinor(int minor)
{
    if (minor < 0 || minor >= MAX_SERIAL_PORTS)
        return NULL;
    return registeredPorts[minor];
}

/* ---- BSD Character Device Callbacks ---- */

static int vserial_open(dev_t dev, int flags, int devtype, proc_t p)
{
    (void)devtype; (void)p;
    int minor_num = minor(dev);
    VirtIOSerialPort *port = VirtIOSerialPort_findByMinor(minor_num);
    if (!port) return ENXIO;
    return port->devOpen(flags);
}

static int vserial_close(dev_t dev, int flags, int devtype, proc_t p)
{
    (void)flags; (void)devtype; (void)p;
    int minor_num = minor(dev);
    VirtIOSerialPort *port = VirtIOSerialPort_findByMinor(minor_num);
    if (!port) return ENXIO;
    return port->devClose();
}

static int vserial_read(dev_t dev, struct uio *uio, int ioflag)
{
    (void)ioflag;
    int minor_num = minor(dev);
    VirtIOSerialPort *port = VirtIOSerialPort_findByMinor(minor_num);
    if (!port) return ENXIO;

    uint32_t len = (uint32_t)uio_resid(uio);
    if (len == 0) return 0;

    /* Cap read size */
    if (len > VIRTIO_SERIAL_BUF_SIZE)
        len = VIRTIO_SERIAL_BUF_SIZE;

    char *buf = (char *)IOMalloc(len);
    if (!buf) return ENOMEM;

    uint32_t bytesRead = 0;
    int ret = port->devRead(buf, len, &bytesRead);

    if (ret == 0 && bytesRead > 0) {
        ret = uiomove(buf, (int)bytesRead, uio);
    }

    IOFree(buf, len);
    return ret;
}

static int vserial_write(dev_t dev, struct uio *uio, int ioflag)
{
    (void)ioflag;
    int minor_num = minor(dev);
    VirtIOSerialPort *port = VirtIOSerialPort_findByMinor(minor_num);
    if (!port) return ENXIO;

    uint32_t len = (uint32_t)uio_resid(uio);
    if (len == 0) return 0;

    if (len > VIRTIO_SERIAL_MAX_WRITE)
        len = VIRTIO_SERIAL_MAX_WRITE;

    char *buf = (char *)IOMalloc(len);
    if (!buf) return ENOMEM;

    int ret = uiomove(buf, (int)len, uio);
    if (ret == 0) {
        ret = port->devWrite(buf, len);
    }

    IOFree(buf, len);
    return ret;
}

static struct cdevsw vserial_cdevsw = {
    .d_open     = vserial_open,
    .d_close    = vserial_close,
    .d_read     = vserial_read,
    .d_write    = vserial_write,
    .d_ioctl    = eno_ioctl,
    .d_stop     = eno_stop,
    .d_reset    = eno_reset,
    .d_ttys     = NULL,
    .d_select   = eno_select,
    .d_mmap     = eno_mmap,
    .d_strategy = eno_strat,
    .d_type     = 0
};

/* ---- Init / Start / Stop ---- */

bool VirtIOSerialPort::init(uint32_t id, const char *name, VirtIOConsole *parent)
{
    if (!super::init())
        return false;

    portId = id;
    strlcpy(portName, name, sizeof(portName));
    console = parent;
    majorNum = -1;
    devNode_cu = NULL;
    devNode_tty = NULL;
    isOpen = false;

    return true;
}

bool VirtIOSerialPort::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    if (!createCharDevice()) {
        IOLog("VirtIOSerialPort: failed to create char device for port %u\n", portId);
        super::stop(provider);
        return false;
    }

    IOLog("VirtIOSerialPort: port %u (%s) started, /dev/cu.%s and /dev/tty.%s created\n",
          portId, portName, portName, portName);
    return true;
}

void VirtIOSerialPort::stop(IOService *provider)
{
    destroyCharDevice();
    super::stop(provider);
}

void VirtIOSerialPort::free()
{
    super::free();
}

/* ---- Character Device Creation ---- */

bool VirtIOSerialPort::createCharDevice()
{
    /* Register cdevsw if not already done */
    if (registeredMajor < 0) {
        registeredMajor = cdevsw_add(-1, &vserial_cdevsw);
        if (registeredMajor < 0) {
            IOLog("VirtIOSerialPort: cdevsw_add failed\n");
            return false;
        }
        IOLog("VirtIOSerialPort: registered cdevsw major=%d\n", registeredMajor);
        bzero(registeredPorts, sizeof(registeredPorts));
    }

    if (portId >= MAX_SERIAL_PORTS) {
        IOLog("VirtIOSerialPort: port %u exceeds max\n", portId);
        return false;
    }

    registeredPorts[portId] = this;

    /* Create /dev/cu.<portName> (callout device) */
    char cu_name[140];
    snprintf(cu_name, sizeof(cu_name), "cu.%s", portName);
    devNode_cu = devfs_make_node(makedev(registeredMajor, portId),
                                 DEVFS_CHAR, UID_ROOT, GID_WHEEL,
                                 0666, "%s", cu_name);

    /* Create /dev/tty.<portName> (dialin device) */
    char tty_name[140];
    snprintf(tty_name, sizeof(tty_name), "tty.%s", portName);
    devNode_tty = devfs_make_node(makedev(registeredMajor, portId + MAX_SERIAL_PORTS),
                                   DEVFS_CHAR, UID_ROOT, GID_WHEEL,
                                   0666, "%s", tty_name);

    if (!devNode_cu) {
        IOLog("VirtIOSerialPort: failed to create /dev/%s\n", cu_name);
        return false;
    }

    IOLog("VirtIOSerialPort: created /dev/%s and /dev/%s\n", cu_name, tty_name);
    return true;
}

void VirtIOSerialPort::destroyCharDevice()
{
    if (devNode_cu) {
        devfs_remove(devNode_cu);
        devNode_cu = NULL;
    }
    if (devNode_tty) {
        devfs_remove(devNode_tty);
        devNode_tty = NULL;
    }

    if (portId < MAX_SERIAL_PORTS) {
        registeredPorts[portId] = NULL;
    }
}

/* ---- Device Operations ---- */

int VirtIOSerialPort::devOpen(int flags)
{
    (void)flags;
    if (isOpen) return 0; /* Allow reopen */

    isOpen = true;

    /* Notify host that port is open */
    if (console)
        console->portOpened(portId);

    IOLog("VirtIOSerialPort: port %u opened\n", portId);
    return 0;
}

int VirtIOSerialPort::devClose()
{
    if (!isOpen) return 0;

    isOpen = false;

    if (console)
        console->portClosed(portId);

    IOLog("VirtIOSerialPort: port %u closed\n", portId);
    return 0;
}

int VirtIOSerialPort::devRead(void *buf, uint32_t len, uint32_t *bytesRead)
{
    if (!console) return EIO;
    return console->readPort(portId, buf, len, bytesRead);
}

int VirtIOSerialPort::devWrite(const void *buf, uint32_t len)
{
    if (!console) return EIO;
    int ret = console->writePort(portId, buf, len);
    return ret < 0 ? EIO : 0;
}
