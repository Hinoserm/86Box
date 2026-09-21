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
#include <86box/nvr.h>
#include <86box/machine.h>
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
    uint8_t embedded_id[4]; /* a device soldered to the board */

    /* The EISA configuration RAM: thirty-two pages of two hundred and
       fifty-six bytes, seen through a window at 0800h with the page
       chosen by CONFRAMP at 0C00h. This is where the configuration
       utility leaves what it worked out and where the BIOS looks at
       every power on, so it has to outlive the machine. */
    uint8_t cram[ESC_CRAM_PAGES * 256];
    uint8_t cram_page;
    uint8_t cram_auto; /* rebuild it when the cards change */
    uint8_t cram_sig[(EISA_MAX_SLOTS + 1) * 4];   /* what the file was written for */
    uint8_t cram_checked;                         /* the cards have been looked at */
    char    cram_file[128];

    /* The I/O APIC: a select register and a window onto the identifier,
       the version, the arbitration priority and sixteen redirection
       entries of two words each. */
    mem_mapping_t apic_mapping;
    uint32_t      apic_sel;
    uint32_t      apic_id;
    uint32_t      apic_redir[16][2];

    port_92_t *port_92;
} esc_t;

typedef struct pceb_t {
    uint8_t pci_slot;
    uint8_t irq_state;
    uint8_t regs[256];
} pceb_t;

static esc_t *esc_inst = NULL;

/* ------------------------------------------------------------------ */
/* The fail-safe timer                                                 */
/* ------------------------------------------------------------------ */

/* Timer 2, counter 0 is the fail-safe timer: software is meant to keep
   reloading it, and if it ever runs out the board takes that as the
   machine having wedged and raises an NMI. Bit 2 of the extended NMI
   control register is what lets it through, and the data book is explicit
   that the status bit only sets when it is enabled. */
static void
esc_fail_safe_timer(int new_out, int old_out, UNUSED(void *priv))
{
    esc_t *dev = esc_inst;

    if ((dev == NULL) || !new_out || old_out)
        return;

    if (!(dev->nmi_esc & 0x04))
        return;

    esc_log("ESC: the fail-safe timer ran out\n");
    dev->nmi_esc |= 0x80;
    nmi           = 1;
    nmi_auto_clear = 1;
}

/* ------------------------------------------------------------------ */
/* PCI interrupt steering                                              */
/* ------------------------------------------------------------------ */

/* A PIRQ reaches an IRQ only when its own route register says so and the
   mode select register has the PIRQ pins switched on at all: bit 6 there
   is what makes them PIRQs rather than MREQs. */
static void
esc_pirq_update(esc_t *dev)
{
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t route = dev->regs[0x60 + i];

        if ((dev->regs[0x40] & 0x40) && !(route & 0x80))
            pci_set_irq_routing(PCI_INTA + i, route & 0x0f);
        else
            pci_set_irq_routing(PCI_INTA + i, PCI_IRQ_DISABLED);
    }
}

/* ------------------------------------------------------------------ */
/* The I/O APIC                                                        */
/* ------------------------------------------------------------------ */

/* The register file is all here and behaves as the data book says. What
   is not here is anywhere for a message to go: the emulator has no local
   APIC and no APIC bus, so an unmasked entry is remembered and nothing
   more. A machine with one processor runs through the 8259s, which is
   what INTRC = 0 in the PAC register means, and never notices. */
#define ESC_APIC_VER      0x000f0011
#define ESC_APIC_LO_MASK  0x0001afff /* bits 12 and 14 are the chip's */
#define ESC_APIC_HI_MASK  0xff000000

static uint32_t
esc_apic_reg_read(esc_t *dev)
{
    uint8_t reg = dev->apic_sel & 0xff;

    switch (reg) {
        case 0x00: /* APICID */
        case 0x02: /* APICARB, which a write to the identifier also loads */
            return dev->apic_id;

        case 0x01: /* APICVER */
            return ESC_APIC_VER;

        case 0x10 ... 0x2f:
            return dev->apic_redir[(reg - 0x10) >> 1][reg & 1];

        default:
            break;
    }

    return 0x00000000;
}

static void
esc_apic_reg_write(esc_t *dev, uint32_t val)
{
    uint8_t reg = dev->apic_sel & 0xff;

    switch (reg) {
        case 0x00:
            dev->apic_id = val & 0x0f000000;
            break;

        case 0x10 ... 0x2f:
            if (reg & 1)
                dev->apic_redir[(reg - 0x10) >> 1][1] = val & ESC_APIC_HI_MASK;
            else {
                uint32_t *lo = &dev->apic_redir[(reg - 0x10) >> 1][0];

                *lo = (*lo & ~ESC_APIC_LO_MASK) | (val & ESC_APIC_LO_MASK);
                esc_log("ESC: I/O APIC entry %i = %08x%s\n", (reg - 0x10) >> 1,
                        *lo, (*lo & 0x00010000) ? "" : " (unmasked, and no "
                        "local APIC to take it)");
            }
            break;

        default:
            break;
    }
}

static uint32_t
esc_apic_readl(uint32_t addr, void *priv)
{
    esc_t *dev = (esc_t *) priv;

    switch (addr & 0x3fc) {
        case 0x000:
            return dev->apic_sel;
        case 0x010:
            return esc_apic_reg_read(dev);
        default:
            break;
    }

    return 0xffffffff;
}

static void
esc_apic_writel(uint32_t addr, uint32_t val, void *priv)
{
    esc_t *dev = (esc_t *) priv;

    switch (addr & 0x3fc) {
        case 0x000:
            dev->apic_sel = val & 0x000000ff;
            break;
        case 0x010:
            esc_apic_reg_write(dev, val);
            break;
        default:
            break;
    }
}

/* Software is told to use doubleword accesses. Narrower ones are put
   together from those rather than refused. */
static uint8_t
esc_apic_readb(uint32_t addr, void *priv)
{
    return (uint8_t) (esc_apic_readl(addr, priv) >> ((addr & 3) * 8));
}

static uint16_t
esc_apic_readw(uint32_t addr, void *priv)
{
    return (uint16_t) (esc_apic_readl(addr, priv) >> ((addr & 2) * 8));
}

static void
esc_apic_writeb(uint32_t addr, uint8_t val, void *priv)
{
    uint32_t cur   = esc_apic_readl(addr, priv);
    int      shift = (addr & 3) * 8;

    cur = (cur & ~(0xffU << shift)) | ((uint32_t) val << shift);
    esc_apic_writel(addr, cur, priv);
}

static void
esc_apic_writew(uint32_t addr, uint16_t val, void *priv)
{
    uint32_t cur   = esc_apic_readl(addr, priv);
    int      shift = (addr & 2) * 8;

    cur = (cur & ~(0xffffU << shift)) | ((uint32_t) val << shift);
    esc_apic_writel(addr, cur, priv);
}

/* APICBASE moves the unit in 1 KB steps: bits 5:2 are address bits 15:12
   and, on the SB part, bits 1:0 are address bits 11:10. */
static void
esc_apic_remap(esc_t *dev)
{
    uint32_t base = 0xfec00000 |
                    (((uint32_t) (dev->regs[0x59] >> 2) & 0x0f) << 12) |
                    (((uint32_t) dev->regs[0x59] & 0x03) << 10);

    mem_mapping_set_addr(&dev->apic_mapping, base, 0x400);
    esc_log("ESC: I/O APIC at %08x\n", base);
}

static void
esc_apic_reset(esc_t *dev)
{
    dev->apic_sel = 0x00000000;
    dev->apic_id  = 0x00000000;

    /* Every pin starts masked. */
    for (uint8_t i = 0; i < 16; i++) {
        dev->apic_redir[i][0] = 0x00010000;
        dev->apic_redir[i][1] = 0x00000000;
    }
}

/* ------------------------------------------------------------------ */
/* The EISA configuration RAM                                          */
/* ------------------------------------------------------------------ */

/* A configuration record as the utility and the BIOS both understand it:
   the four identifier bytes, a byte saying the slot is in use and what
   kind of board it is, and then the function records. Firmware checks
   the identifier against what the slot actually answers, which is why
   generating this from the machine rather than from a file is safe --
   the two cannot disagree. This is only the starting point: once the
   BIOS or the utility has written the store, what they wrote is kept. */
static void
esc_cram_checksum(uint8_t *page)
{
    uint16_t sum = 0;
    uint8_t  csum = 0;

    /* The sum over the record, excluding the two checksum bytes, is what
       the BIOS compares against. */
    page[2] = page[3] = 0;
    for (uint16_t i = 0; i < 256; i++)
        sum = (uint16_t) (sum + page[i]);
    sum     = (uint16_t) (0 - sum);
    page[2] = (uint8_t) (sum & 0xff);
    page[3] = (uint8_t) (sum >> 8);
}

/* Build the configuration from the machine as it stands. Slot zero is the
   board itself; the rest are whatever answered when the bus was walked. */
static void
esc_cram_generate(esc_t *dev)
{
    uint8_t *c = dev->cram;
    uint16_t sum  = 0;
    uint8_t  csum = 0;

    memset(c, 0, sizeof(dev->cram));

    /* The layout the board's own firmware writes, which is the only one it
       will accept. Everything it cares about is reached through a base it
       keeps at offset eight, so the words below that are a wrapper and the
       configuration proper starts at 50h.

       Read out of a store the BIOS had just written, and confirmed by
       disassembling its run-time module: the byte reader puts the page in
       CONFRAMP and the offset in the window, the block reader adds 50h to
       every offset it is given, and the check it runs is a plain byte sum
       over the region whose length sits at base+4. */
    c[0x00] = 0x55;
    c[0x01] = 0xaa;
    /* c[4] is the store's own checksum and is filled in at the end. */
    c[0x06] = 0x42;
    c[0x08] = ESC_CRAM_BASE;    /* where the block starts   */
    c[0x0a] = 0x40;             /* where the ESCD starts    */
    c[0x0b] = 0x01;
    c[0x0c] = 0x05;
    c[0x0d] = 0x13;
    c[0x0e] = 0x10;
    c[0x0f] = 0xac;
    c[0x11] = 0x58;
    c[0x40] = 0x0b;
    c[0x41] = 0x02;

    /* The configuration block. base+4 is how much of it is summed, and the
       sum goes in base+2. */
    c[ESC_CRAM_BASE + 4] = 0xb0; /* length, 1fb0h */
    c[ESC_CRAM_BASE + 5] = 0x1f;
    c[ESC_CRAM_BASE + 6] = 0xb4;
    c[ESC_CRAM_BASE + 7] = 0x1e;
    c[ESC_CRAM_BASE + 10] = 0xfc;

    /* Whose it is. */
    memcpy(&c[0xf0], "AMI", 3);

    /* The extended configuration data, exactly the fourteen bytes the
       firmware copies in when it starts a store from nothing. */
    {
        static const uint8_t escd[14] = {
            0x0e, 0x00, 'A', 'C', 'F', 'G', 0x01, 0x02,
            0x00, 0x00, 0x00, 0x00, 0xde, 0xfe
        };
        memcpy(&c[ESC_CRAM_ESCD], escd, sizeof(escd));
    }

    /* A device soldered to the board is recorded even with nothing in any
       slot: on this one that is the Adaptec, which the firmware writes as
       ADP7880 whatever the board identifier says. */
    c[0x2e] = 0x05;
    memcpy(&c[0x2f], dev->embedded_id, 4);
    c[0x33] = 0x50;

    /* Then a configuration record for every slot that has a board in it,
       laid out the way the firmware's own "write slot configuration" lays
       them out: a word of length plus four, a word of index, and then the
       twelve bytes the EISA services hand over -- slot information, the
       revision, a checksum, how many functions there are, and the four
       identifier bytes. The records grow upward from block+FCh, which is
       where block+0Ah points and what block+06h counts down from. */
    {
        uint16_t at    = ESC_CRAM_RECS;
        uint16_t free  = 0x1eb4;
        uint8_t  count = 0;

        for (uint8_t slot = 1; slot <= EISA_MAX_SLOTS; slot++) {
            uint8_t *r;

            if (!eisa_slot_occupied(slot) || (free < ESC_CRAM_RECLEN + 4))
                continue;

            r = &c[ESC_CRAM_BASE + at];
            r[0] = ESC_CRAM_RECLEN + 4;
            r[1] = 0x00;
            r[2] = count;
            r[3] = 0x00;

            r[4] = slot;  /* which slot this describes */
            r[5] = 0x01;  /* the revision of the record */
            r[6] = 0x00;
            r[7] = 0x00;  /* its own checksum, filled in below */
            r[8] = 0x00;
            r[9] = 0x00;  /* no function records follow */
            r[10] = 0x00;
            for (uint8_t i = 0; i < 4; i++)
                r[11 + i] = eisa_slot_id(slot, i);

            /* The pointer table, the count, the next free byte and what is
               left of the room. */
            c[ESC_CRAM_BASE + 0x0e + (count * 2)]     = (uint8_t) (at & 0xff);
            c[ESC_CRAM_BASE + 0x0e + (count * 2) + 1] = (uint8_t) (at >> 8);

            at   = (uint16_t) (at + ESC_CRAM_RECLEN + 4);
            free = (uint16_t) (free - (ESC_CRAM_RECLEN + 4));
            count++;
        }

        c[ESC_CRAM_BASE + 0x0c] = count;
        c[ESC_CRAM_BASE + 0x0a] = (uint8_t) (at & 0xff);
        c[ESC_CRAM_BASE + 0x0b] = (uint8_t) (at >> 8);
        c[ESC_CRAM_BASE + 0x06] = (uint8_t) (free & 0xff);
        c[ESC_CRAM_BASE + 0x07] = (uint8_t) (free >> 8);
    }

    /* The block keeps a sixteen bit sum of itself, from base+4 to the end
       of the region whose length sits at base+4. */
    for (uint16_t i = ESC_CRAM_BASE + 4; i < sizeof(dev->cram); i++)
        sum = (uint16_t) (sum + c[i]);
    c[ESC_CRAM_BASE + 2] = (uint8_t) (sum & 0xff);
    c[ESC_CRAM_BASE + 3] = (uint8_t) (sum >> 8);

    /* And the store as a whole carries one byte at offset four chosen so
       that every byte in it adds up to nothing. The firmware zeroes that
       byte, sums the lot and writes back the negation; anything else and
       it throws the store away and builds its own. */
    c[0x04] = 0x00;
    for (uint16_t i = 0; i < sizeof(dev->cram); i++)
        csum = (uint8_t) (csum + c[i]);
    c[0x04] = (uint8_t) (-csum);

    esc_log("ESC: configuration RAM built, block sum %04x, store byte %02x\n",
            sum, c[0x04]);
}

static void
esc_cram_signature(const esc_t *dev, uint8_t *sig)
{
    memcpy(sig, dev->board_id, 4);

    for (uint8_t slot = 1; slot <= EISA_MAX_SLOTS; slot++) {
        uint8_t *ent = sig + (slot * 4);

        if (eisa_slot_occupied(slot))
            for (uint8_t i = 0; i < 4; i++)
                ent[i] = eisa_slot_id(slot, i);
        else
            memset(ent, 0xff, 4);
    }
}

static void
esc_cram_load(esc_t *dev)
{
    FILE *fp;

    fp = nvr_fopen(dev->cram_file, "rb");
    if (fp == NULL) {
        /* Nothing saved yet. With automatic configuration off the guest
           gets an empty store and may do as it likes with it. */
        if (dev->cram_auto)
            esc_cram_generate(dev);
        return;
    }

    if (fread(dev->cram, 1, sizeof(dev->cram), fp) != sizeof(dev->cram)) {
        fclose(fp);
        esc_cram_generate(dev);
        return;
    }

    /* The set of cards the file was written for is kept after the pages.
       Nothing but us reads it: the window is only ever 8 KB wide, so the
       guest never sees it. */
    memset(dev->cram_sig, 0x00, sizeof(dev->cram_sig));
    if (fread(dev->cram_sig, 1, sizeof(dev->cram_sig), fp) !=
        sizeof(dev->cram_sig))
        memset(dev->cram_sig, 0x00, sizeof(dev->cram_sig));

    fclose(fp);
}

/* The cards are plugged in after the chip set is built, so what is in the
   slots cannot be known at init. The first look at the configuration RAM
   is late enough, and the BIOS does not touch it before then. */
static void
esc_cram_verify(esc_t *dev)
{
    uint8_t sig[(EISA_MAX_SLOTS + 1) * 4];

    if (dev->cram_checked)
        return;
    dev->cram_checked = 1;

    esc_cram_signature(dev, sig);

    /* Manual mode keeps whatever is in the file, the way a real board keeps
       whatever the utility last wrote. Automatic mode keeps it too, right
       up until the cards change: then it is thrown away, so the BIOS finds
       an empty store and asks for the utility -- exactly what a real board
       does when someone opens it up and moves a card. */
    if (dev->cram_auto && memcmp(dev->cram_sig, sig, sizeof(sig))) {
        esc_log("ESC: the cards have changed, starting the configuration "
                "RAM again\n");
        esc_cram_generate(dev);
    }

    memcpy(dev->cram_sig, sig, sizeof(sig));
}

static void
esc_cram_save(esc_t *dev)
{
    FILE *fp;

    /* With the machine's automatic configuration switched off the store is
       the guest's business alone, and we do not write the file. */
    if (!dev->cram_auto)
        return;

    /* If the guest never looked at the store the signature is still the
       one the file came with, which is what should go back. */
    esc_cram_verify(dev);

    fp = nvr_fopen(dev->cram_file, "wb");
    if (fp == NULL)
        return;

    fwrite(dev->cram, 1, sizeof(dev->cram), fp);
    fwrite(dev->cram_sig, 1, sizeof(dev->cram_sig), fp);
    fclose(fp);
}

/* ------------------------------------------------------------------ */
/* The board identifier                                               */
/* ------------------------------------------------------------------ */

void
esc_set_embedded_id(const char *mfg, uint16_t product, uint8_t rev)
{
    if (esc_inst != NULL)
        eisa_make_id(esc_inst->embedded_id, mfg, product, rev);
}

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
            dev->regs[0x40] = val & 0x7f;
            esc_log("ESC: mode select %02x, PIRQs %s\n", val,
                    (val & 0x40) ? "on" : "off");
            esc_pirq_update(dev);
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
            dev->regs[0x59] = val & 0x3f;
            esc_apic_remap(dev);
            break;

        case 0x60: /* PIRQRC0..3 */
        case 0x61:
        case 0x62:
        case 0x63:
            dev->regs[index] = val & 0x8f;
            esc_pirq_update(dev);
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

        case 0x70: /* PAC, PCI/APIC control: INTR and SMI routing */
            dev->regs[0x70] = val & 0x03;
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
            /* Bit 0 drives RSTDRV, which resets everything on the bus.
               Software holds it for a few clocks and takes it away again;
               the cards see the edge, so reset them as it goes on. */
            if (val & 0x01) {
                esc_log("ESC: RSTDRV, resetting the bus\n");
                eisa_reset();
            }
            break;

        case 0x0462: /* SOFTNMI, a write of any value is the event */
            if (dev->nmi_esc & 0x02) {
                dev->nmi_esc |= 0x20;
                nmi = 1;
            }
            break;

        case 0x0c00: /* CONFRAMP, which page of the configuration RAM */
            dev->cram_page = val & 0x1f;
            break;

        case 0x0800 ... 0x08ff: /* the configuration RAM itself */
            esc_cram_verify(dev);
            esc_log("ESC: cram wr %02x:%02x = %02x\n", dev->cram_page,
                    port & 0xff, val);
            dev->cram[(dev->cram_page * 256) + (port & 0xff)] = val;
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

        case 0x0c00:
            return dev->cram_page;

        case 0x0800 ... 0x08ff:
            esc_cram_verify((esc_t *) dev);
            esc_log("ESC: cram rd %02x:%02x = %02x\n", dev->cram_page,
                    port & 0xff,
                    dev->cram[(dev->cram_page * 256) + (port & 0xff)]);
            return dev->cram[(dev->cram_page * 256) + (port & 0xff)];

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

    esc_pirq_update(dev);

    esc_apic_reset(dev);
    esc_apic_remap(dev);

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

    esc_cram_save(dev);

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
    dma_eisa_init();
    dma_set_sg_base(0x04);

    /* Two interrupt controllers whose trigger can be chosen per line,
       which is what EISA needs and what ISA never had. */
    pic_elcr_set_enabled(1);
    pic_elcr_io_handler(1);

    /* A second timer: counter 0 is the fail-safe timer that can raise
       NMI, counter 2 drives CPU speed control. */
    device_add(&i8254_sec_device);

    /* Timer 2's first counter is the fail-safe timer, and its output is
       an NMI source rather than an interrupt. */
    if (pit_devs[1].data != NULL)
        pit_devs[1].set_out_func(pit_devs[1].data, 0, esc_fail_safe_timer);

    dev->port_92 = device_add(&port_92_pci_device);

    io_sethandler(ESC_CONF_INDEX, 0x0002, esc_read, NULL, NULL, esc_write,
                  NULL, NULL, dev);
    io_sethandler(0x0461, 0x0002, esc_read, NULL, NULL, esc_write, NULL, NULL,
                  dev);
    io_sethandler(0x0464, 0x0001, esc_read, NULL, NULL, esc_write, NULL, NULL,
                  dev);
    io_sethandler(0x0c80, 0x0004, esc_read, NULL, NULL, esc_write, NULL, NULL,
                  dev);
    io_sethandler(0x0800, 0x0100, esc_read, NULL, NULL, esc_write, NULL, NULL,
                  dev);
    io_sethandler(0x0c00, 0x0001, esc_read, NULL, NULL, esc_write, NULL, NULL,
                  dev);

    mem_mapping_add(&dev->apic_mapping, 0xfec00000, 0x400,
                    esc_apic_readb, esc_apic_readw, esc_apic_readl,
                    esc_apic_writeb, esc_apic_writew, esc_apic_writel,
                    NULL, MEM_MAPPING_EXTERNAL, dev);

    /* The machine owns this now: "Auto EISA Config" on its options page.
       On, the store is built fresh whenever it is missing or the cards
       have changed. Off, nothing here writes to it at all. */
    dev->cram_auto = (uint8_t) machine_get_config_int("auto_eisa_config");
    snprintf(dev->cram_file, sizeof(dev->cram_file), "%s_eisa.nvr",
             machine_get_internal_name());
    esc_cram_load(dev);

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

    pci_add_card(PCI_ADD_SOUTHBRIDGE, pceb_read, pceb_write, dev, &dev->pci_slot);

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
