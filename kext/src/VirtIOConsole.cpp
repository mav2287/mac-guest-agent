#include "VirtIOConsole.h"
#include "VirtIOSerialPort.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/OSAtomic.h>

#define super VirtIOPCIDevice
OSDefineMetaClassAndStructors(VirtIOConsole, VirtIOPCIDevice)

/* ---- Probe ---- */

IOService *VirtIOConsole::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return NULL;

    uint16_t vendorId = pci->configRead16(kIOPCIConfigVendorID);
    uint16_t deviceId = pci->configRead16(kIOPCIConfigDeviceID);

    if (vendorId != VIRTIO_PCI_VENDOR_ID)
        return NULL;

    if (deviceId != VIRTIO_PCI_DEVICE_CONSOLE_LEGACY &&
        deviceId != VIRTIO_PCI_DEVICE_CONSOLE_MODERN)
        return NULL;

    IOLog("VirtIOConsole: probe matched vendor=0x%04x device=0x%04x\n", vendorId, deviceId);

    /* High probe score to override Apple's driver if present */
    *score = 5000;
    return this;
}

/* ---- Start ---- */

bool VirtIOConsole::start(IOService *provider)
{
    IOLog("VirtIOConsole: starting\n");

    bzero(ports, sizeof(ports));
    multiport = false;
    maxPorts = 1;
    intSource = NULL;
    timerSource = NULL;
    ctrlSendBuf = NULL;
    ctrl_recv_count = 0;

    /* Initialize PCI transport (maps BAR0) */
    if (!super::start(provider))
        return false;

    /* Reset device */
    resetDevice();

    /* Set ACKNOWLEDGE */
    addStatus(VIRTIO_STATUS_ACKNOWLEDGE);

    /* Set DRIVER */
    addStatus(VIRTIO_STATUS_DRIVER);

    /* Negotiate features */
    if (!negotiateFeatures()) {
        IOLog("VirtIOConsole: feature negotiation failed\n");
        resetDevice();
        super::stop(provider);
        return false;
    }

    /* Initialize virtqueues */
    if (!initQueues()) {
        IOLog("VirtIOConsole: queue initialization failed\n");
        resetDevice();
        super::stop(provider);
        return false;
    }

    /* Set DRIVER_OK */
    addStatus(VIRTIO_STATUS_DRIVER_OK);
    IOLog("VirtIOConsole: device status = 0x%02x (DRIVER_OK)\n", getStatus());

    /* Start multiport protocol if supported */
    if (multiport) {
        if (!startMultiport()) {
            IOLog("VirtIOConsole: multiport init failed, falling back to single port\n");
            multiport = false;
        }
    }

    /* If single-port mode, create port 0 directly */
    if (!multiport) {
        addPort(0);
        setPortName(0, "org.qemu.guest_agent.0");
        setPortOpen(0, true);
    }

    /* Set up interrupt handling */
    workLoop = getWorkLoop();
    if (workLoop) {
        intSource = IOFilterInterruptEventSource::filterInterruptEventSource(
            this,
            (IOInterruptEventAction)&VirtIOConsole::interruptAction,
            (IOFilterInterruptAction)&VirtIOConsole::interruptFilter,
            provider, 0
        );

        if (intSource && workLoop->addEventSource(intSource) == kIOReturnSuccess) {
            intSource->enable();
            IOLog("VirtIOConsole: interrupt handler registered\n");
        } else {
            IOLog("VirtIOConsole: interrupt setup failed, using timer fallback\n");
            if (intSource) {
                intSource->release();
                intSource = NULL;
            }
        }

        /* Timer as fallback/supplement for polling */
        timerSource = IOTimerEventSource::timerEventSource(
            this, (IOTimerEventSource::Action)&VirtIOConsole::timerFired
        );
        if (timerSource && workLoop->addEventSource(timerSource) == kIOReturnSuccess) {
            timerSource->setTimeoutMS(100); /* 100ms polling interval */
        }
    }

    /* Register ourselves so children can find us */
    registerService();

    IOLog("VirtIOConsole: started successfully, %u ports, multiport=%s\n",
          maxPorts, multiport ? "yes" : "no");
    return true;
}

/* ---- Stop ---- */

void VirtIOConsole::stop(IOService *provider)
{
    IOLog("VirtIOConsole: stopping\n");

    if (timerSource) {
        timerSource->cancelTimeout();
        workLoop->removeEventSource(timerSource);
        timerSource->release();
        timerSource = NULL;
    }

    if (intSource) {
        intSource->disable();
        workLoop->removeEventSource(intSource);
        intSource->release();
        intSource = NULL;
    }

    /* Remove all ports */
    for (uint32_t i = 0; i < MAX_PORTS; i++) {
        if (ports[i].active)
            removePort(i);
    }

    /* Free control queues */
    if (multiport) {
        for (int i = 0; i < ctrl_recv_count; i++) {
            if (ctrl_recv_bufs[i])
                IOFree(ctrl_recv_bufs[i], VIRTIO_SERIAL_BUF_SIZE);
        }
        controlRecvQ.free();
        controlSendQ.free();
    }

    if (ctrlSendBuf) {
        ctrlSendBuf->complete();
        ctrlSendBuf->release();
        ctrlSendBuf = NULL;
    }

    super::stop(provider);
}

void VirtIOConsole::free()
{
    super::free();
}

/* ---- Feature Negotiation ---- */

bool VirtIOConsole::negotiateFeatures()
{
    deviceFeatures = readDeviceFeatures();
    IOLog("VirtIOConsole: device features = 0x%08x\n", deviceFeatures);

    driverFeatures = 0;

    if (deviceFeatures & VIRTIO_CONSOLE_F_MULTIPORT) {
        driverFeatures |= VIRTIO_CONSOLE_F_MULTIPORT;
        multiport = true;
        IOLog("VirtIOConsole: MULTIPORT feature negotiated\n");
    }

    if (deviceFeatures & VIRTIO_CONSOLE_F_SIZE) {
        driverFeatures |= VIRTIO_CONSOLE_F_SIZE;
    }

    writeDriverFeatures(driverFeatures);

    /* For legacy devices, FEATURES_OK isn't required */
    addStatus(VIRTIO_STATUS_FEATURES_OK);
    if (!(getStatus() & VIRTIO_STATUS_FEATURES_OK)) {
        IOLog("VirtIOConsole: FEATURES_OK not set (legacy device, continuing)\n");
        /* Legacy devices don't have FEATURES_OK - this is fine */
    }

    return true;
}

/* ---- Queue Initialization ---- */

bool VirtIOConsole::initQueues()
{
    if (multiport) {
        /* Read max_nr_ports from device config */
        maxPorts = readConfig32(offsetof(struct virtio_console_config, max_nr_ports));
        if (maxPorts == 0 || maxPorts > MAX_PORTS)
            maxPorts = MAX_PORTS;
        IOLog("VirtIOConsole: max_nr_ports = %u\n", maxPorts);
    } else {
        maxPorts = 1;
    }

    /* Queue layout: Q0/Q1 = port 0, Q2/Q3 = control (if multiport), Q4/Q5 = port 1, ... */

    /* Set up port 0 queues */
    ports[0].receiveq_idx = 0;
    ports[0].transmitq_idx = 1;
    if (!setupQueue(0, &ports[0].receiveq) || !setupQueue(1, &ports[0].transmitq)) {
        IOLog("VirtIOConsole: failed to set up port 0 queues\n");
        return false;
    }

    if (multiport) {
        /* Control queues at indices 2 and 3 */
        controlRecvQIdx = 2;
        controlSendQIdx = 3;
        if (!setupQueue(2, &controlRecvQ) || !setupQueue(3, &controlSendQ)) {
            IOLog("VirtIOConsole: failed to set up control queues\n");
            return false;
        }

        /* Set up remaining port queues */
        for (uint32_t i = 1; i < maxPorts; i++) {
            uint16_t base = (uint16_t)(4 + (i - 1) * 2);
            ports[i].receiveq_idx = base;
            ports[i].transmitq_idx = base + 1;
            if (!setupQueue(base, &ports[i].receiveq) ||
                !setupQueue(base + 1, &ports[i].transmitq)) {
                IOLog("VirtIOConsole: failed to set up port %u queues\n", i);
                /* Non-fatal: port just won't be available */
            }
        }
    }

    return true;
}

/* ---- Multiport Protocol ---- */

bool VirtIOConsole::startMultiport()
{
    /* Allocate control send buffer */
    ctrlSendBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        VIRTIO_SERIAL_BUF_SIZE,
        0xFFFFF000ULL
    );
    if (!ctrlSendBuf) {
        IOLog("VirtIOConsole: failed to allocate control send buffer\n");
        return false;
    }
    ctrlSendBuf->prepare();

    /* Pre-fill control receive queue */
    fillControlReceiveQueue();

    /* Send DEVICE_READY to host */
    sendControlMsg(VIRTIO_CONSOLE_BAD_ID, VIRTIO_CONSOLE_DEVICE_READY, 1);
    IOLog("VirtIOConsole: sent DEVICE_READY\n");

    /* Process any initial control messages (ports, names, etc.) */
    /* Give the host a moment, then poll */
    IODelay(10000); /* 10ms */
    processControlMessages();

    return true;
}

void VirtIOConsole::fillControlReceiveQueue()
{
    for (int i = 0; i < 16 && controlRecvQ.getFreeCount() > 0; i++) {
        IOBufferMemoryDescriptor *bufDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous,
            VIRTIO_SERIAL_BUF_SIZE,
            0xFFFFF000ULL
        );
        if (!bufDesc) break;
        bufDesc->prepare();

        void *buf = bufDesc->getBytesNoCopy();
        uint64_t phys = bufDesc->getPhysicalAddress();

        ctrl_recv_bufs[i] = buf;
        ctrl_recv_phys[i] = phys;
        ctrl_recv_count = i + 1;

        controlRecvQ.addInputBuffer(buf, VIRTIO_SERIAL_BUF_SIZE, phys);
    }

    notifyQueue(controlRecvQIdx);
    IOLog("VirtIOConsole: pre-filled control receive queue with %d buffers\n", ctrl_recv_count);
}

void VirtIOConsole::sendControlMsg(uint32_t id, uint16_t event, uint16_t value)
{
    if (!ctrlSendBuf) return;

    struct virtio_console_control *ctrl =
        (struct virtio_console_control *)ctrlSendBuf->getBytesNoCopy();
    ctrl->id = id;
    ctrl->event = event;
    ctrl->value = value;

    uint64_t phys = ctrlSendBuf->getPhysicalAddress();
    controlSendQ.addOutputBuffer(ctrl, sizeof(*ctrl), phys);
    notifyQueue(controlSendQIdx);

    /* Spin-wait for host to consume (matches Linux behavior) */
    uint32_t len;
    int timeout = 100000; /* ~100ms at 1us per iteration */
    while (controlSendQ.getUsedBuffer(&len) < 0 && --timeout > 0) {
        IODelay(1);
    }

    if (timeout <= 0) {
        IOLog("VirtIOConsole: control message send timeout (event=%u)\n", event);
    }
}

void VirtIOConsole::processControlMessages()
{
    uint32_t len;
    int idx;

    while ((idx = controlRecvQ.getUsedBuffer(&len)) >= 0) {
        if (idx < ctrl_recv_count && ctrl_recv_bufs[idx]) {
            void *buf = ctrl_recv_bufs[idx];

            if (len >= sizeof(struct virtio_console_control)) {
                struct virtio_console_control *ctrl = (struct virtio_console_control *)buf;
                const void *extra = (const uint8_t *)buf + sizeof(*ctrl);
                uint32_t extraLen = len - sizeof(*ctrl);

                handleControlMessage(ctrl, extra, extraLen);
            }

            /* Re-queue the buffer */
            controlRecvQ.addInputBuffer(buf, VIRTIO_SERIAL_BUF_SIZE, ctrl_recv_phys[idx]);
        }
    }

    notifyQueue(controlRecvQIdx);
}

void VirtIOConsole::handleControlMessage(struct virtio_console_control *ctrl,
                                          const void *extra, uint32_t extraLen)
{
    IOLog("VirtIOConsole: control msg: id=%u event=%u value=%u\n",
          ctrl->id, ctrl->event, ctrl->value);

    switch (ctrl->event) {
    case VIRTIO_CONSOLE_DEVICE_READY:
        IOLog("VirtIOConsole: host reports DEVICE_READY\n");
        break;

    case VIRTIO_CONSOLE_PORT_ADD:
        if (ctrl->id < MAX_PORTS) {
            addPort(ctrl->id);
            sendControlMsg(ctrl->id, VIRTIO_CONSOLE_PORT_READY, 1);
        }
        break;

    case VIRTIO_CONSOLE_PORT_REMOVE:
        if (ctrl->id < MAX_PORTS)
            removePort(ctrl->id);
        break;

    case VIRTIO_CONSOLE_PORT_NAME:
        if (ctrl->id < MAX_PORTS && extra && extraLen > 0) {
            /* Extract null-terminated name string */
            char name[128];
            uint32_t nameLen = extraLen < 127 ? extraLen : 127;
            memcpy(name, extra, nameLen);
            name[nameLen] = '\0';
            /* Strip trailing null/whitespace */
            while (nameLen > 0 && (name[nameLen - 1] == '\0' || name[nameLen - 1] == '\n'))
                name[--nameLen] = '\0';

            setPortName(ctrl->id, name);
        }
        break;

    case VIRTIO_CONSOLE_PORT_OPEN:
        if (ctrl->id < MAX_PORTS)
            setPortOpen(ctrl->id, ctrl->value != 0);
        break;

    case VIRTIO_CONSOLE_CONSOLE_PORT:
        /* We don't need HVC console support, just serial ports */
        IOLog("VirtIOConsole: port %u is a console port (ignoring)\n", ctrl->id);
        break;

    case VIRTIO_CONSOLE_RESIZE:
        /* Not relevant for serial ports */
        break;

    default:
        IOLog("VirtIOConsole: unknown control event %u\n", ctrl->event);
        break;
    }
}

/* ---- Port Management ---- */

void VirtIOConsole::addPort(uint32_t id)
{
    if (id >= MAX_PORTS) return;
    if (ports[id].active) return;

    ports[id].active = true;
    ports[id].host_connected = false;
    ports[id].name[0] = '\0';
    ports[id].serialPort = NULL;
    ports[id].recv_buf_count = 0;

    /* Pre-fill receive queue */
    fillReceiveQueue(&ports[id]);

    IOLog("VirtIOConsole: port %u added\n", id);
}

void VirtIOConsole::removePort(uint32_t id)
{
    if (id >= MAX_PORTS || !ports[id].active) return;

    /* Terminate child serial port */
    if (ports[id].serialPort) {
        ports[id].serialPort->terminate();
        ports[id].serialPort->release();
        ports[id].serialPort = NULL;
    }

    /* Free receive buffers */
    for (int i = 0; i < ports[id].recv_buf_count; i++) {
        if (ports[id].recv_bufs[i])
            IOFree(ports[id].recv_bufs[i], VIRTIO_SERIAL_BUF_SIZE);
    }

    ports[id].receiveq.free();
    ports[id].transmitq.free();
    ports[id].active = false;

    IOLog("VirtIOConsole: port %u removed\n", id);
}

void VirtIOConsole::setPortName(uint32_t id, const char *name)
{
    if (id >= MAX_PORTS || !ports[id].active) return;

    strlcpy(ports[id].name, name, sizeof(ports[id].name));
    IOLog("VirtIOConsole: port %u name = \"%s\"\n", id, name);

    /* If this is the guest agent port, create the serial port nub */
    if (strcmp(name, "org.qemu.guest_agent.0") == 0) {
        createSerialPortNub(id);
    }
}

void VirtIOConsole::setPortOpen(uint32_t id, bool open)
{
    if (id >= MAX_PORTS || !ports[id].active) return;

    ports[id].host_connected = open;
    IOLog("VirtIOConsole: port %u host_connected = %s\n", id, open ? "yes" : "no");
}

bool VirtIOConsole::createSerialPortNub(uint32_t id)
{
    if (id >= MAX_PORTS || !ports[id].active) return false;
    if (ports[id].serialPort) return true; /* Already created */

    VirtIOSerialPort *port = new VirtIOSerialPort;
    if (!port) return false;

    if (!port->init(id, ports[id].name, this)) {
        port->release();
        return false;
    }

    if (!port->attach(this)) {
        port->release();
        return false;
    }

    port->registerService();
    ports[id].serialPort = port;

    IOLog("VirtIOConsole: created serial port nub for port %u (%s)\n", id, ports[id].name);
    return true;
}

void VirtIOConsole::fillReceiveQueue(struct VirtIOPort *port)
{
    for (int i = 0; i < 32 && port->receiveq.getFreeCount() > 0; i++) {
        IOBufferMemoryDescriptor *bufDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous,
            VIRTIO_SERIAL_BUF_SIZE,
            0xFFFFF000ULL
        );
        if (!bufDesc) break;
        bufDesc->prepare();

        void *buf = bufDesc->getBytesNoCopy();
        uint64_t phys = bufDesc->getPhysicalAddress();

        port->recv_bufs[i] = buf;
        port->recv_phys[i] = phys;
        port->recv_buf_count = i + 1;

        port->receiveq.addInputBuffer(buf, VIRTIO_SERIAL_BUF_SIZE, phys);
    }

    notifyQueue(port->receiveq_idx);
}

/* ---- Data Path ---- */

int VirtIOConsole::writePort(uint32_t portId, const void *data, uint32_t len)
{
    if (portId >= MAX_PORTS || !ports[portId].active)
        return -1;

    if (len > VIRTIO_SERIAL_MAX_WRITE)
        len = VIRTIO_SERIAL_MAX_WRITE;

    struct VirtIOPort *port = &ports[portId];

    /* Allocate a DMA-capable buffer for the data */
    IOBufferMemoryDescriptor *bufDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionOut | kIOMemoryPhysicallyContiguous,
        len,
        0xFFFFF000ULL
    );
    if (!bufDesc) return -1;
    bufDesc->prepare();

    memcpy(bufDesc->getBytesNoCopy(), data, len);
    uint64_t phys = bufDesc->getPhysicalAddress();

    int idx = port->transmitq.addOutputBuffer(bufDesc->getBytesNoCopy(), len, phys);
    if (idx < 0) {
        bufDesc->complete();
        bufDesc->release();
        return -1;
    }

    notifyQueue(port->transmitq_idx);

    /* Wait for completion */
    uint32_t usedLen;
    int timeout = 500000; /* ~500ms */
    while (port->transmitq.getUsedBuffer(&usedLen) < 0 && --timeout > 0) {
        IODelay(1);
    }

    bufDesc->complete();
    bufDesc->release();

    return timeout > 0 ? (int)len : -1;
}

int VirtIOConsole::readPort(uint32_t portId, void *buf, uint32_t len, uint32_t *bytesRead)
{
    if (portId >= MAX_PORTS || !ports[portId].active)
        return -1;

    *bytesRead = 0;
    struct VirtIOPort *port = &ports[portId];

    uint32_t usedLen;
    int idx = port->receiveq.getUsedBuffer(&usedLen);
    if (idx < 0)
        return 0; /* No data available */

    if (idx < port->recv_buf_count && port->recv_bufs[idx]) {
        uint32_t copyLen = usedLen < len ? usedLen : len;
        memcpy(buf, port->recv_bufs[idx], copyLen);
        *bytesRead = copyLen;

        /* Re-queue the receive buffer */
        port->receiveq.addInputBuffer(port->recv_bufs[idx], VIRTIO_SERIAL_BUF_SIZE,
                                       port->recv_phys[idx]);
        notifyQueue(port->receiveq_idx);
    }

    return 0;
}

void VirtIOConsole::portOpened(uint32_t portId)
{
    if (multiport && portId < MAX_PORTS) {
        sendControlMsg(portId, VIRTIO_CONSOLE_PORT_OPEN, 1);
    }
}

void VirtIOConsole::portClosed(uint32_t portId)
{
    if (multiport && portId < MAX_PORTS) {
        sendControlMsg(portId, VIRTIO_CONSOLE_PORT_OPEN, 0);
    }
}

/* ---- Interrupt Handling ---- */

bool VirtIOConsole::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src)
{
    VirtIOConsole *self = (VirtIOConsole *)owner;
    if (!self || !self->bar0Base) return false;

    uint8_t isr = self->readISR();
    return (isr & (VIRTIO_ISR_QUEUE | VIRTIO_ISR_CONFIG)) != 0;
}

void VirtIOConsole::interruptAction(OSObject *owner, IOFilterInterruptEventSource *src, int count)
{
    VirtIOConsole *self = (VirtIOConsole *)owner;
    if (self) self->handleInterrupt();
}

void VirtIOConsole::handleInterrupt()
{
    /* Process control messages */
    if (multiport) {
        processControlMessages();
    }

    /* Process all port receive queues — wake any waiting readers */
    for (uint32_t i = 0; i < maxPorts; i++) {
        if (!ports[i].active) continue;

        /* Check for completed transmit buffers */
        uint32_t len;
        while (ports[i].transmitq.getUsedBuffer(&len) >= 0) {
            /* Transmit completed — buffer already freed in writePort */
        }
    }
}

/* Timer fallback for polling */
void VirtIOConsole::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
    VirtIOConsole *self = (VirtIOConsole *)owner;
    if (!self) return;

    self->handleInterrupt();

    /* Re-arm timer */
    if (sender)
        sender->setTimeoutMS(100);
}
