/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Intel's PCI-EISA bridge chip set: the 82375EB/82375SB
 *          PCI-EISA Bridge (PCEB) and the 82374EB/82374SB EISA System
 *          Component (ESC).
 *
 *          The two are one bridge in two packages. The PCEB faces the
 *          PCI bus, is the only half with a PCI configuration header,
 *          and owns the address paths: what memory and I/O the EISA side
 *          may reach on PCI, and what PCI may reach on EISA. The ESC
 *          faces the EISA bus and is everything a south bridge normally
 *          is -- two interrupt controllers, two DMA controllers, the
 *          timers, the NMI logic -- with the EISA extensions layered on
 *          top: thirty-two bit addresses, scatter-gather, per channel
 *          stop registers, and the slot decode that lets firmware find a
 *          card without probing for it.
 *
 *          On a board with no PIIX, and the AIR 54TDP is one, the ESC is
 *          the south bridge outright and its PIRQ route registers are the
 *          only thing steering PCI interrupts.
 *
 *          The ESC has no PCI presence at all. Its configuration space is
 *          reached through an index and data pair at 0022h and 0023h, and
 *          stays shut after reset until 0Fh is written to the ID register
 *          at index 02h; other devices of the day indexed the same ports,
 *          and that byte is how the ESC knows it is the one being spoken
 *          to.
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/timer.h>
#include <86box/pci.h>
#include <86box/dma.h>
#include <86box/pic.h>
#include <86box/pit.h>
#include <86box/nmi.h>
#include <86box/port_92.h>
#include <86box/eisa.h>
#include <86box/esc.h>
#include <86box/plat_unused.h>

#ifdef ENABLE_ESC_LOG
int esc_do_log = ENABLE_ESC_LOG;

static void
esc_log(const char *fmt, ...)
{
    va_list ap;

    if (esc_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define esc_log(fmt, ...)
#endif

typedef struct esc_t {
    uint8_t index;    /* what 0022h last selected */
    uint8_t unlocked; /* 0Fh has been written to the ID register */
    uint8_t regs[256];

    uint8_t nmi_esc;  /* 0461h, extended NMI status and control */
    uint8_t last_mst; /* 0464h, last EISA bus master granted */

    uint8_t board_id[4];

    port_92_t *port_92;
} esc_t;

typedef struct pceb_t {
    uint8_t pci_slot;
    uint8_t irq_state;
    uint8_t regs[256];
} pceb_t;

static esc_t *esc_inst = NULL;

/* ------------------------------------------------------------------ */
/* The board identifier                                               */
/* ------------------------------------------------------------------ */

void
esc_set_board_id(const char *mfg, uint16_t product, uint8_t rev)
{
    uint8_t id[4];

    eisa_make_id(id, mfg, product, rev);

    if (esc_inst != NULL) {
        memcpy(esc_inst->board_id, id, 4);
        /* The identifier registers and the I/O window are the same four
           bytes seen from two sides. */
        for (uint8_t i = 0; i < 4; i++)
            esc_inst->regs[0x50 + i] = id[i];
    }
    eisa_set_board_id(id);
}

/* ------------------------------------------------------------------ */
/* ESC configuration space                                            */
/* ------------------------------------------------------------------ */

static void
esc_conf_write(esc_t *dev, uint8_t index, uint8_t val)
{
    /* Until the ID register has been given 0Fh the part is deaf to
       everything else. */
    if (!dev->unlocked && (index != 0x02))
        return;

    switch (index) {
        case 0x02: /* ESCID */
            dev->regs[0x02] = val;
            dev->unlocked   = (val == ESC_UNLOCK_KEY);
            esc_log("ESC: id %02x, %s\n", val,
                    dev->unlocked ? "unlocked" : "locked");
            break;

        case 0x08: /* RID, read only */
            break;

        case 0x40: /* MS, mode select */
            /* Bits 1:0 choose how many slots the part decodes for, which
               is what decides whether the address enables are direct or
               encoded. Everything here is the direct four slot case. */
            dev->regs[0x40] = val;
            break;

        case 0x42: /* BIOSCSA */
        case 0x43: /* BIOSCSB */
            dev->regs[index] = val;
            break;

        case 0x4d: /* CLKDIV */
            dev->regs[0x4d] = val & 0x73;
            break;

        case 0x4e: /* PCSA */
        case 0x4f: /* PCSB */
            dev->regs[index] = val;
            break;

        case 0x50: /* EISAID1..4 */
        case 0x51:
        case 0x52:
        case 0x53:
            dev->regs[index]            = val;
            dev->board_id[index - 0x50] = val;
            eisa_set_board_id(dev->board_id);
            break;

        case 0x57: /* SGRBA, where the scatter-gather registers live */
            dev->regs[0x57] = val;
            dma_remove_sg();
            dma_set_sg_base(val);
            break;

        case 0x59: /* APICBA */
            dev->regs[0x59] = val & 0xfc;
            break;

        case 0x60: /* PIRQRC0..3 */
        case 0x61:
        case 0x62:
        case 0x63:
            dev->regs[index] = val & 0x8f;
            if (val & 0x80)
                pci_set_irq_routing(PCI_INTA + (index & 0x03), PCI_IRQ_DISABLED);
            else
                pci_set_irq_routing(PCI_INTA + (index & 0x03), val & 0x0f);
            esc_log("ESC: PIRQ %c -> %02x\n", 'A' + (index & 3), val);
            break;

        /* Three general purpose chip selects, each a low and high address
           and a mask. Nothing on this side of the bridge acts on them;
           they drive pins. */
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x68:
        case 0x69:
        case 0x6a:
        case 0x6c:
        case 0x6d:
        case 0x6e:
        case 0x6f: /* GPXBC */
            dev->regs[index] = val;
            break;

        case 0x70: /* PACC, PIC/APIC configuration control */
            dev->regs[0x70] = val & 0x1f;
            break;

        case 0x88: /* TSTC, test control */
            dev->regs[0x88] = val;
            break;

        case 0xa0: /* SMICNTL */
            dev->regs[0xa0] = val & 0x0f;
            break;
        case 0xa2: /* SMIEN */
        case 0xa3:
            dev->regs[index] = val;
            break;
        case 0xa4: /* SEE */
        case 0xa5:
        case 0xa6:
        case 0xa7:
            dev->regs[index] = val;
            break;
        case 0xa8: /* FTMR */
            dev->regs[0xa8] = val;
            break;
        case 0xaa: /* SMIREQ, write one to clear */
        case 0xab:
            dev->regs[index] &= ~val;
            break;
        case 0xac: /* CTLTMRL */
        case 0xae: /* CTLTMRH */
            dev->regs[index] = val;
            break;

        default:
            /* Reserved. Writes have no effect. */
            break;
    }
}

static uint8_t
esc_conf_read(esc_t *dev, uint8_t index)
{
    if (!dev->unlocked && (index != 0x02))
        return 0x00;

    switch (index) {
        case 0x02:
        case 0x08:
        case 0x40:
        case 0x42:
        case 0x43:
        case 0x4d:
        case 0x4e:
        case 0x4f:
        case 0x50:
        case 0x51:
        case 0x52:
        case 0x53:
        case 0x57:
        case 0x59:
        case 0x60:
        case 0x61:
        case 0x62:
        case 0x63:
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x68:
        case 0x69:
        case 0x6a:
        case 0x6c:
        case 0x6d:
        case 0x6e:
        case 0x6f:
        case 0x70:
        case 0x88:
        case 0xa0:
        case 0xa2:
        case 0xa3:
        case 0xa4:
        case 0xa5:
        case 0xa6:
        case 0xa7:
        case 0xa8:
        case 0xaa:
        case 0xab:
        case 0xac:
        case 0xae:
            return dev->regs[index];

        default:
            /* A reserved location reads as zero and still completes. */
            return 0x00;
    }
}

/* ------------------------------------------------------------------ */
/* ESC I/O                                                            */
/* ------------------------------------------------------------------ */

static void
esc_write(uint16_t port, uint8_t val, void *priv)
{
    esc_t *dev = (esc_t *) priv;

    switch (port) {
        case ESC_CONF_INDEX:
            dev->index = val;
            break;
        case ESC_CONF_DATA:
            esc_conf_write(dev, dev->index, val);
            break;

        case 0x0461: /* NMIESC, extended NMI status and control */
            /* Bits 7:4 are status and read only; the rest are controls,
               and clearing an enable also clears the condition it
               reported. */
            dev->nmi_esc = (uint8_t) ((dev->nmi_esc & 0xf0) | (val & 0x0f));
            if (!(val & 0x02))
                dev->nmi_esc &= ~0x20;
            if (!(val & 0x04))
                dev->nmi_esc &= ~0x80;
            if (!(val & 0x08))
                dev->nmi_esc &= ~0x50;
            break;

        case 0x0462: /* SOFTNMI, a write of any value is the event */
            if (dev->nmi_esc & 0x02) {
                dev->nmi_esc |= 0x20;
                nmi = 1;
            }
            break;

        case 0x0c80: /* the board identifier is read only from here */
        case 0x0c81:
        case 0x0c82:
        case 0x0c83:
            break;

        default:
            break;
    }
}

static uint8_t
esc_read(uint16_t port, void *priv)
{
    const esc_t *dev = (esc_t *) priv;

    switch (port) {
        case ESC_CONF_INDEX:
            return dev->index;
        case ESC_CONF_DATA:
            return esc_conf_read((esc_t *) dev, dev->index);

        case 0x0461:
            return dev->nmi_esc;

        case 0x0464:
            /* Which EISA master was granted the bus last. Nothing here
               arbitrates, so it is whatever was recorded. */
            return dev->last_mst;

        case 0x0c80:
        case 0x0c81:
        case 0x0c82:
        case 0x0c83:
            return dev->board_id[port & 3];

        default:
            return 0xff;
    }
}

static void
esc_reset_hard(esc_t *dev)
{
    memset(dev->regs, 0, sizeof(dev->regs));

    dev->index    = 0;
    dev->unlocked = 0;

    /* B-0 stepping. The EB is 02h. */
    dev->regs[0x08] = 0x03;
    /* The scatter-gather registers default to the 04xx page. */
    dev->regs[0x57] = 0x04;
    /* Every PCI interrupt starts unrouted. */
    dev->regs[0x60] = dev->regs[0x61] = 0x80;
    dev->regs[0x62] = dev->regs[0x63] = 0x80;

    dev->nmi_esc  = 0x00;
    dev->last_mst = 0x00;

    for (uint8_t i = 0; i < 4; i++)
        dev->regs[0x50 + i] = dev->board_id[i];

    dma_remove_sg();
    dma_set_sg_base(0x04);

    pci_set_irq_routing(PCI_INTA, PCI_IRQ_DISABLED);
    pci_set_irq_routing(PCI_INTB, PCI_IRQ_DISABLED);
    pci_set_irq_routing(PCI_INTC, PCI_IRQ_DISABLED);
    pci_set_irq_routing(PCI_INTD, PCI_IRQ_DISABLED);

    eisa_reset();
}

static void
esc_reset(void *priv)
{
    esc_reset_hard((esc_t *) priv);
}

static void
esc_close(void *priv)
{
    esc_t *dev = (esc_t *) priv;

    if (esc_inst == dev)
        esc_inst = NULL;
    free(dev);
}

static void *
esc_init(UNUSED(const device_t *info))
{
    esc_t *dev = (esc_t *) calloc(1, sizeof(esc_t));

    esc_inst = dev;

    /* The compatible half of the part. 86Box already models the pieces;
       what the ESC adds is that they are all in one place and that the
       EISA extensions are switched on. */
    dma_set_params(1, 0xffffffff);
    dma_ext_mode_init();
    dma_high_page_init();
    dma_set_sg_base(0x04);

    /* Two interrupt controllers whose trigger can be chosen per line,
       which is what EISA needs and what ISA never had. */
    pic_elcr_set_enabled(1);

    /* A second timer: counter 0 is the fail-safe timer that can raise
       NMI, counter 2 drives CPU speed control. */
    device_add(&i8254_sec_device);

    dev->port_92 = device_add(&port_92_pci_device);

    io_sethandler(ESC_CONF_INDEX, 0x0002, esc_read, NULL, NULL, esc_write,
                  NULL, NULL, dev);
    io_sethandler(0x0461, 0x0002, esc_read, NULL, NULL, esc_write, NULL, NULL,
                  dev);
    io_sethandler(0x0464, 0x0001, esc_read, NULL, NULL, esc_write, NULL, NULL,
                  dev);
    io_sethandler(0x0c80, 0x0004, esc_read, NULL, NULL, esc_write, NULL, NULL,
                  dev);

    esc_reset_hard(dev);

    return dev;
}

const device_t esc_device = {
    .name          = "Intel 82374SB (ESC)",
    .internal_name = "esc",
    .flags         = DEVICE_EISA,
    .local         = 0x03,
    .init          = esc_init,
    .close         = esc_close,
    .reset         = esc_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

/* ------------------------------------------------------------------ */
/* PCEB — the PCI facing half                                         */
/* ------------------------------------------------------------------ */

/* The four memory and four I/O windows that let an EISA bus master reach
   PCI. Each memory region is four bytes: a base and a size, both in the
   units the register defines. */
static void
pceb_recalc_regions(UNUSED(pceb_t *dev))
{
    /* The forwarding decision is made when a cycle is run, not here; the
       registers are kept so firmware reads back what it wrote and so the
       decode can consult them. Memory forwarding on this bridge is a
       positive decode and 86Box's EISA masters already reach memory
       through the same physical address space, so nothing has to be
       remapped for it to work. */
}

static void
pceb_write(int func, int addr, UNUSED(int len), uint8_t val, void *priv)
{
    pceb_t *dev = (pceb_t *) priv;

    if (func > 0)
        return;

    switch (addr) {
        case 0x04: /* PCICMD */
            dev->regs[0x04] = val & 0x47;
            break;
        case 0x05:
            dev->regs[0x05] = val & 0x01;
            break;

        case 0x07: /* PCISTS, write one to clear */
            dev->regs[0x07] &= (uint8_t) ~(val & 0xf9);
            break;

        case 0x0d: /* MLTIM */
            dev->regs[0x0d] = val & 0xf8;
            break;

        case 0x40: /* PCICON */
            dev->regs[0x40] = val;
            break;
        case 0x41: /* ARBCON */
            dev->regs[0x41] = val;
            break;
        case 0x42: /* ARBPRI */
            dev->regs[0x42] = val;
            break;
        case 0x43: /* ARBPRIX */
            dev->regs[0x43] = val;
            break;

        case 0x44: /* MCSCON */
        case 0x45: /* MCSBOH */
        case 0x46: /* MCSTOH */
        case 0x47: /* MCSTOM */
            dev->regs[addr] = val;
            break;

        case 0x48: /* EADC1 */
        case 0x49:
            dev->regs[addr] = val;
            break;

        case 0x4c: /* IORTC */
            dev->regs[0x4c] = val;
            break;

        case 0x54: /* MAR1..3 */
        case 0x55:
        case 0x56:
            dev->regs[addr] = val;
            break;

        case 0x58: /* PDCON */
            dev->regs[0x58] = val;
            break;

        case 0x5a: /* EADC2 */
            dev->regs[0x5a] = val;
            break;

        case 0x5c: /* EPMRA */
            dev->regs[0x5c] = val;
            break;

        /* EISA-to-PCI memory regions 1 through 4, four bytes each. */
        case 0x60 ... 0x6f:
            dev->regs[addr] = val;
            pceb_recalc_regions(dev);
            break;

        /* EISA-to-PCI I/O regions 1 through 4. */
        case 0x70 ... 0x7f:
            dev->regs[addr] = val;
            pceb_recalc_regions(dev);
            break;

        case 0x80: /* BTMR, the BIOS timer's base address */
        case 0x81:
            dev->regs[addr] = val;
            break;

        case 0x84: /* ELTCR */
            dev->regs[0x84] = val;
            break;

        /* 88-8B is the test control register, which the datasheet says
           plainly must not be written. Reads are allowed. */
        default:
            break;
    }
}

static uint8_t
pceb_read(int func, int addr, UNUSED(int len), void *priv)
{
    const pceb_t *dev = (pceb_t *) priv;

    if (func > 0)
        return 0xff;

    return dev->regs[addr & 0xff];
}

static void
pceb_reset_hard(pceb_t *dev)
{
    memset(dev->regs, 0, sizeof(dev->regs));

    dev->regs[0x00] = 0x86; /* Intel */
    dev->regs[0x01] = 0x80;
    dev->regs[0x02] = 0x82; /* 82375EB/SB */
    dev->regs[0x03] = 0x04;
    dev->regs[0x04] = 0x07; /* I/O, memory and bus mastering all on */
    dev->regs[0x06] = 0x80;
    dev->regs[0x07] = 0x02; /* medium DEVSEL */
    dev->regs[0x08] = 0x04; /* revision */
    dev->regs[0x09] = 0x00;
    dev->regs[0x0a] = 0x00; /* bridge, EISA */
    dev->regs[0x0b] = 0x06;

    dev->regs[0x40] = 0x00;
    dev->regs[0x41] = 0x00;
    dev->regs[0x42] = 0x00;
    dev->regs[0x43] = 0x00;
    dev->regs[0x44] = 0x00;
    dev->regs[0x47] = 0x00;
    dev->regs[0x4c] = 0x00;
}

static void
pceb_reset(void *priv)
{
    pceb_reset_hard((pceb_t *) priv);
}

static void
pceb_close(void *priv)
{
    free(priv);
}

static void *
pceb_init(UNUSED(const device_t *info))
{
    pceb_t *dev = (pceb_t *) calloc(1, sizeof(pceb_t));

    pceb_reset_hard(dev);

    pci_add_card(PCI_ADD_NORMAL, pceb_read, pceb_write, dev, &dev->pci_slot);

    return dev;
}

const device_t pceb_device = {
    .name          = "Intel 82375SB (PCEB)",
    .internal_name = "pceb",
    .flags         = DEVICE_PCI,
    .local         = 0x04,
    .init          = pceb_init,
    .close         = pceb_close,
    .reset         = pceb_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};
