/* SPDX-License-Identifier: GPL-2.0-or-later */
/* LS26 MUSB A-device. Firmware schedules transactions using endpoint CSRs.
 * EP0, bulk and interrupt tokens are delivered to the attached USBDevice. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "hw/riscv/arcs/arcs_usb_host.h"
#include "hw/usb.h"
#include "qemu/bswap.h"

typedef struct ArcsUSBDownstream {
    USBBus bus;
    USBPort port;
    USBPacket packet;
    QEMUTimer *timer;
    uint8_t targets[16][8];
    bool multipoint[16][2];
} ArcsUSBDownstream;

#define MICROFRAME_NS 125000
#define CSR0_REQ 0x20
#define RX_REQ 0x20

static void port_attach(USBPort *port)
{
    ArcsUSB *s = port->opaque;
    if (s->session) {
        s->interrupt |= 0x10;
        arcs_usb_irq(s);
    }
}

static void cancel(ArcsUSB *s)
{
    arcs_usb_fifo_clear(s);
    memset(s->endpoint, 0, sizeof(s->endpoint));
    memset(s->downstream->targets, 0, sizeof(s->downstream->targets));
    memset(s->downstream->multipoint, 0, sizeof(s->downstream->multipoint));
    s->tx_pending = s->rx_pending = 0;
    s->address = 0;
    s->host->frame = 0;
    timer_del(s->downstream->timer);
}

static void port_detach(USBPort *port)
{
    ArcsUSB *s = port->opaque;
    cancel(s);
    s->power &= ~0x10;
    if (s->session) {
        s->interrupt |= 0x20;
        arcs_usb_irq(s);
    }
}

static USBPortOps port_ops = {.attach = port_attach, .detach = port_detach};
static USBBusOps bus_ops;

static int token(ArcsUSB *s, unsigned local, int pid, void *buf, unsigned size)
{
    ArcsUSBDownstream *d = s->downstream;
    USBDevice *dev = d->port.dev;
    bool in = pid == USB_TOKEN_IN;
    unsigned type = local ? s->endpoint[local][in ? 12 : 10] : 0;
    unsigned slot = local && in;
    unsigned addr = d->multipoint[local][slot] ? d->targets[local][slot * 4] : s->address;
    if (!(s->power & 0x10) || !dev || !dev->attached || dev->state != USB_STATE_DEFAULT || dev->addr != addr) {
        return USB_RET_NODEV;
    }
    USBEndpoint *ep = usb_ep_get(dev, pid, type & 15);
    if (local && ((type >> 4 & 3) < 2 || !ep || ep->type != (type >> 4 & 3))) {
        return USB_RET_STALL;
    }
    USBPacket *p = &d->packet;
    usb_packet_setup(p, pid, ep, 0, 0, false, false);
    usb_packet_addbuf(p, buf, size);
    usb_handle_packet(dev, p);
    /* Only the synchronous board device may occupy this dedicated port. */
    if (usb_packet_is_inflight(p)) {
        usb_cancel_packet(p);
        return USB_RET_IOERROR;
    }
    return p->status == USB_RET_SUCCESS ? p->actual_length : p->status;
}

static void ep0(ArcsUSB *s)
{
    uint8_t *csr = &s->endpoint[0][2];
    bool in = !!(*csr & CSR0_REQ);
    if (!in && !(*csr & 2)) {
        return;
    }
    int result = token(s, 0, in ? USB_TOKEN_IN : (*csr & 8) ? USB_TOKEN_SETUP : USB_TOKEN_OUT,
                       in ? s->rx_fifo : s->tx_fifo,
                       in ? ((*csr & 0x40) ? 0 : sizeof(s->rx_fifo)) : s->tx_count);
    if (result == USB_RET_NAK) {
        return;
    }
    s->tx_count = s->rx_position = s->rx_count = 0;
    *csr = 0;
    if (result < 0) {
        *csr = result == USB_RET_STALL ? 4 : 0x10;
    } else if (in) {
        *csr = 1;
        s->rx_count = result;
    }
    s->tx_pending |= 1;
}

static void endpoint(ArcsUSB *s, unsigned ep, bool in)
{
    uint8_t *regs = s->endpoint[ep], *csr = &regs[in ? 6 : 2];
    unsigned request = in ? RX_REQ : 1;
    if (!(*csr & request) || (in && (*csr & 1))) {
        return;
    }
    unsigned max = lduw_le_p(regs + (in ? 4 : 0)) & 0x7ff;
    unsigned size = in ? max : s->bulk_tx_count[ep];
    int result = !max || max > sizeof(s->bulk_rx[ep]) || size > max ? USB_RET_IOERROR :
        token(s, ep, in ? USB_TOKEN_IN : USB_TOKEN_OUT, in ? s->bulk_rx[ep] : s->bulk_tx[ep], size);
    if (result == USB_RET_NAK) {
        return;
    }
    *csr &= ~request;
    if (result < 0) {
        *csr |= result == USB_RET_STALL ? (in ? 0x40 : 0x20) : 4;
    } else if (in) {
        s->bulk_count[ep] = result;
        s->bulk_position[ep] = 0;
        *csr |= 1;
        regs[7] ^= 2; /* Host RX data toggle */
    } else {
        s->bulk_tx_count[ep] = 0;
        *csr &= ~2;
        regs[3] ^= 1; /* Host TX data toggle */
    }
    if (in) {
        s->rx_pending |= 1U << ep;
    } else {
        s->tx_pending |= 1U << ep;
    }
}

void arcs_usb_downstream_kick(ArcsUSB *s)
{
    if (s->downstream && s->session && !timer_pending(s->downstream->timer)) {
        timer_mod(s->downstream->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MICROFRAME_NS);
    }
}

static void service(void *opaque)
{
    ArcsUSB *s = opaque;
    if (!s->session || (s->power & 8)) {
        return;
    }
    arcs_usb_dma_service(s);
    ep0(s);
    bool pending = s->endpoint[0][2] & (2 | CSR0_REQ);
    for (unsigned ep = 1; ep < 16; ep++) {
        endpoint(s, ep, false);
        endpoint(s, ep, true);
    }
    arcs_usb_dma_service(s);
    for (unsigned ep = 1; ep < 16; ep++) {
        pending |= !!((s->endpoint[ep][2] & 1) | (s->endpoint[ep][6] & RX_REQ));
    }
    if (pending) {
        arcs_usb_downstream_kick(s);
    }
    arcs_usb_irq(s);
}

uint8_t arcs_usb_downstream_devctl(ArcsUSB *s)
{
    USBDevice *dev = s->downstream->port.dev;
    /* A high-speed peripheral initially presents a full-speed pull-up
     * before reset/chirp negotiation. */
    bool full = dev && dev->attached && !(s->power & 0x10);
    return s->session ? 0x1d | (full ? 0x40 : 0) : 0;
}

bool arcs_usb_downstream_write(ArcsUSB *s, unsigned ep, unsigned off, uint8_t byte)
{
    ArcsUSBDownstream *d = s->downstream;
    USBDevice *dev = d->port.dev;
    uint8_t *regs = s->endpoint[ep];
    if (off >= 0x80 && off < 0x100) {
        unsigned local = (off - 0x80) / 8, reg = off & 7;
        if (reg == 0 || reg == 4) {
            d->targets[local][reg] = byte & 127;
            d->multipoint[local][reg / 4] = true;
        } else if (byte) {
            arcs_soc_fail(s->soc, 0x41000000 + off, 1, true, byte);
        }
        return true;
    }
    switch (off) {
    case 1:
        if ((s->power ^ byte) & 8) {
            cancel(s);
            s->power &= ~0x10;
            if (!(byte & 8) && dev && dev->attached && (byte & 0x20)) {
                usb_device_reset(dev);
                s->power |= 0x10;
            }
        }
        s->power = (byte & 0x29) | (s->power & 0x10);
        arcs_usb_sof_update(s->host);
        return true;
    case 0x60:
        if (!!(byte & 1) != s->session) {
            s->session = byte & 1;
            cancel(s);
            if (s->session && dev && dev->attached) {
                s->interrupt |= 0x10;
            }
            if (!s->session) {
                usb_device_reset(dev);
                s->power &= ~0x10;
            }
            arcs_usb_sof_update(s->host);
        }
        return true;
    case 0x12:
        if (!ep) {
            if (!(byte & 1)) {
                s->rx_count = s->rx_position = 0;
            }
            regs[2] = (byte & 0x6a) | (regs[2] & byte & 0x95);
        } else {
            if (byte & 8) {
                s->bulk_tx_count[ep] = 0;
            }
            if (byte & 0x40) {
                regs[3] &= ~1;
            }
            regs[2] = (regs[2] & byte & 0xa4) | (byte & 1);
        }
        break;
    case 0x16:
        if (ep && ((byte & 0x10) || !(byte & 1))) {
            s->bulk_count[ep] = s->bulk_position[ep] = 0;
        }
        if (byte & 0x80) {
            regs[7] &= ~2;
        }
        regs[6] = (regs[6] & byte & 0x4d) | (byte & 0x20);
        break;
    case 0x13: case 0x17:
        /* Toggle write enable gates firmware changes to the data toggle. */
        if (ep) {
            unsigned toggle = off == 0x13 ? 1 : 2;
            unsigned enable = toggle << 1;
            uint8_t old = regs[off - 0x10];
            regs[off - 0x10] = (byte & ~toggle) | ((byte & enable ? byte : old) & toggle);
        }
        break;
    case 0x1a: case 0x1b: case 0x1c: case 0x1d:
        regs[off - 0x10] = byte;
        break;
    default:
        return false;
    }
    arcs_usb_downstream_kick(s);
    return true;
}

uint8_t arcs_usb_downstream_target(ArcsUSB *s, unsigned off)
{
    return s->downstream->targets[(off - 0x80) / 8][off & 7];
}

BusState *arcs_usb_downstream_init(ArcsSoC *soc)
{
    ArcsUSB *s = &soc->usb;
    ArcsUSBDownstream *d = g_new0(ArcsUSBDownstream, 1);
    s->downstream = d;
    usb_bus_new(&d->bus, sizeof(d->bus), &bus_ops, DEVICE(soc));
    usb_register_port(&d->bus, &d->port, s, 0, &port_ops, USB_SPEED_MASK_HIGH);
    usb_packet_init(&d->packet);
    d->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, service, s);
    return BUS(&d->bus);
}

void arcs_usb_downstream_reset(ArcsUSB *s)
{
    if (s->downstream) {
        cancel(s);
        usb_device_reset(s->downstream->port.dev);
    }
}

void arcs_usb_downstream_finalize(ArcsUSB *s)
{
    if (s->downstream) {
        ArcsUSBDownstream *d = s->downstream;
        usb_bus_release(&d->bus);
        usb_packet_cleanup(&d->packet);
        timer_free(d->timer);
        g_free(d);
        s->downstream = NULL;
    }
}
