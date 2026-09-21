/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Machines with both PCI and EISA.
 *
 *          The combination was never common and did not last: EISA was
 *          the server bus of the early nineties and PCI replaced it from
 *          underneath, so the boards that carried both were built in the
 *          few years when a buyer might already own EISA cards worth
 *          keeping. The AIR 54TDP is one of them -- a 430HX Socket 7
 *          board, dual capable, with four EISA slots, four PCI slots, no
 *          ISA at all, and an Adaptec AIC-7880 soldered on.
 *
 *          What makes it unusual to emulate is that it has no PIIX. The
 *          430HX's normal partner is the 82371SB, but this board pairs
 *          the 82439HX with Intel's PCI-EISA bridge instead, and so the
 *          ESC is the south bridge outright: the interrupt controllers,
 *          the DMA controllers, the timers and the PCI interrupt routing
 *          are all in it.
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/pci.h>
#include <86box/timer.h>
#include <86box/fdd.h>
#include <86box/fdc.h>
#include <86box/fdc_ext.h>
#include <86box/hdc.h>
#include <86box/sio.h>
#include <86box/chipset.h>
#include <86box/eisa.h>
#include <86box/esc.h>
#include <86box/scsi_aic7880.h>
#include <86box/machine.h>
#include <86box/rom.h>
#include <86box/keyboard.h>

int
machine_at_54tdp_init(const machine_t *model)
{
    int ret;

    /* Two builds of the same BIOS exist: 1.03 for one processor and 1.51
       for two. This machine is the uniprocessor one, so it gets 1.03;
       nothing here would know what to do with the second socket. */
    ret = bios_load_linear("roms/machines/54tdp/54tdp103.rom",
                           0x000e0000, 131072, 0);

    if (bios_only || !ret)
        return ret;

    machine_at_common_init(model);

    pci_init(PCI_CONFIG_TYPE_1);
    pci_register_slot(0x00, PCI_CARD_NORTHBRIDGE, 0, 0, 0, 0);
    /* The bridge sits where a south bridge would, because on this board
       it is one. */
    pci_register_slot(0x07, PCI_CARD_SOUTHBRIDGE, 0, 0, 0, 0);
    /* The on-board SCSI, then the four slots. */
    pci_register_slot(0x08, PCI_CARD_SCSI, 1, 2, 3, 4);
    pci_register_slot(0x09, PCI_CARD_VIDEO, 2, 3, 4, 1);
    pci_register_slot(0x0a, PCI_CARD_NORMAL, 3, 4, 1, 2);
    pci_register_slot(0x0b, PCI_CARD_NORMAL, 4, 1, 2, 3);
    pci_register_slot(0x0c, PCI_CARD_NORMAL, 1, 2, 3, 4);
    pci_register_slot(0x0d, PCI_CARD_NORMAL, 2, 3, 4, 1);

    /* Four EISA slots. */
    eisa_init(4);

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);

    device_add(&i430hx_device);
    device_add(&pceb_device);
    device_add(&esc_device);
    device_add(&fdc37c669_device);

    /* The board identifier the BIOS reads out of the ESC. */
    esc_set_board_id("AIR", 0x0901, 0);

    /* The Adaptec is not optional on this board: it is soldered to it,
       and the system BIOS carries its option ROM. */
    device_add(&aic7880_pci_device);

    return ret;
}
