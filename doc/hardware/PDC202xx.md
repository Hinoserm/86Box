# Promise PDC202xx parallel ATA controllers: a hardware reference

This document describes the programming model of Promise Technology's PDC202xx
family of PCI parallel-ATA controllers, from the PDC20246 (Ultra33) to the
PDC20271 (FastTrak TX2000): PCI identity, configuration registers, the
Promise registers beside the SFF-8038i bus master, the indexed register file
and PLL of the "TX2" generation, transfer-timing encodings, the behaviour of
Promise's option ROMs and Windows drivers, the RAID (FastTrak) variants, and
what an emulation of the parts has to get right.

**No datasheet for any of these parts was ever published.** Everything here is
derived from disassembly of Promise's own option ROMs and Windows drivers,
cross-checked against the Linux drivers, and, where marked, against what an
emulation built from this material was observed to need when real firmware
and drivers ran on it.

---

## Contents

1. [Sourcing convention](#1-sourcing-convention)
2. [Sources](#2-sources)
3. [The family](#3-the-family)
4. [PCI configuration space](#4-pci-configuration-space)
5. [The bus-master block and the Promise registers](#5-the-bus-master-block-and-the-promise-registers)
6. [The PLL (TX2 generation)](#6-the-pll-tx2-generation)
7. [Transfer timing](#7-transfer-timing)
8. [Interrupts and DMA](#8-interrupts-and-dma)
9. [Option ROM behaviour](#9-option-rom-behaviour)
10. [Windows driver behaviour](#10-windows-driver-behaviour)
11. [FastTrak (RAID) specifics](#11-fasttrak-raid-specifics)
12. [Emulation notes](#12-emulation-notes)
13. [Open questions and unverified claims](#13-open-questions-and-unverified-claims)
14. [Where the sources disagree](#14-where-the-sources-disagree)

---

## 1. Sourcing convention

Every claim carries one or more tags saying where it comes from.

| Tag | Source | Weight |
|---|---|---|
| **[L]** | The Linux drivers `pata_pdc202xx_old.c`, `pdc202xx_old.c` (a.k.a. `pdc202xx_old_ide.c`), `pdc202xx_new.c`, `pata_pdc2027x.c`. Written with access to Promise and exercised on real silicon for two decades. | Strong for what Linux uses; silent on what it does not. |
| **[C]** | The CORSAC driver `idepdc202xx.cor`, itself derived from [L]. Agreement with [L] is a transcription check, not independent confirmation. | Weak on its own. |
| **[R]** | Disassembly of Promise's option ROMs. | Shows what the chip must return, because firmware branches on it. |
| **[W]** | Disassembly of Promise's Windows drivers (NT-family `.sys` and Windows 9x `.mpd`). | As [R]; several retain the vendor's COFF symbol tables, so routine names are Promise's own. |
| **[E]** | Behaviour found empirically while running the real ROMs and drivers against the 86Box model `hdc_ide_pdc202xx.c` built from this analysis. | Proven necessary for those binaries in that model; not a silicon measurement. |
| **[?]** | Inference. Marked wherever it appears. | Unverified. |

[R] and [W] are the only sources that show what the *chip* does rather than
what one driver chose to do. Locations are given as `FILE offset`, for
example `U133B15.BIN 0x2C65` (a file offset, equal to the CS-relative offset
because the ROMs are linked at origin 0) or `ultra.sys 0x127ca` (a virtual
address; ImageBase is `0x10000` for every PE file here).

---

## 2. Sources

### 2.1 Option ROMs

| File | Part | Version | Size | md5 | Tool |
|---|---|---|---|---|---|
| `Ul200b18.BIN` | PDC20262 Ultra66 | "Ultra66 (tm) BIOS Version 2.00 (Build 18)", 12 Jun 2000 | 16384 | `d64b899e7ae2e6e75a178079bcc83191` | ndisasm |
| `ultra66-bios-v1.14.728-at49f001t.bin` | PDC20262 Ultra66 | "Version 1.14 (Build 0728)"; 16 KiB ROM at offset 0 of a 128 KiB AT49F001T flash image | 131072 | `98817fdff282c9a9a02b835f3c68d79a` | ndisasm |
| `U133B12.BIN` | PDC20269 Ultra133 TX2 | 2.20.0.12 | 16384 | `e3f9a765bb96a05656e02406e0f61e3f` | ndisasm |
| `U133b14.bin` | PDC20269 Ultra133 TX2 | 2.20.0.14 | 16384 | `7d97c42df8faba743b46f283367ea9b9` | ndisasm |
| `U133B15.BIN` | PDC20269 Ultra133 TX2 | "Ultra133TX2 (tm) BIOS Version 2.20.0.15" (string at 0x2330) | 16384 | `d23de8aa988cc65f84de94b9746ea249` | ndisasm |
| `ultra133tx2-bios-v2.20.0050.10-maxtor.bin` | PDC20269 (Maxtor OEM) | 2.20.0050.10 | 16384 | `94b8ebddc3b9a7df658400f65993706a` | ndisasm |
| `U100B15.BIN` | PDC20268 Ultra100 TX2 | 2.20.0.15 | 16384 | `691b52b65c7cdd290f843cf28bd59835` | ndisasm |
| `B14.BIN` | PDC20268 Ultra100 TX2 | 2.20.0.14 | 16384 | `b12037b697f29a302baaeb080c873ae9` | ndisasm |
| `ft100B24.bin` | PDC20267 FastTrak100 | "FastTrak100 (tm) BIOS Version 2.00.0.24", (c) 1995-2001 | 65536 | `adf14aab3d1d75fccedd8a8bfb4ea218` | ndisasm |
| `bios.bin` (FastTrak TX2000) | PDC20271 FastTrak TX2000 | "FastTrak TX2000 (tm) BIOS Version 2.00.0.33", (c) 1995-2002 | 65536 | `6c04cf7a9785128b34cdefe92ade5d69` | ndisasm |

All ROM images carry a whole-image 8-bit checksum of 0x00 where it was
checked (`ft100B24.bin`, TX2000 `bios.bin`).

### 2.2 Windows drivers

| File | Part(s) | Version / timestamp | Size | md5 | Symbols | Tool |
|---|---|---|---|---|---|---|
| `ultra.sys` (WinXP) | Ultra series, gens 1-5 | 2.00.43, Fri 16 May 2003 01:11:00 UTC, linker 5.12, entry 0x154a8 | 35538 | `41202827a5d13905ddd84e9f3219ddfc` | 56 functions | objdump |
| `ultra.sys` (WIN2000) | same | same build | | `264a9f22899fe1ac592cfa0856cc2cf9` | 56 functions | objdump |
| `ultra.sys` (NT4) | same | same build | | `1a906cb5b8f04f990a58235f24907e4e` | 52 functions | objdump |
| `ultra.sys` (Win2003) | same | same build | | `b533ab54973675455c452dc509c6478b` | stripped | objdump |
| `ultra.mpd` (Win9x/Me) | Ultra series, gens 1-5 | 2.00.43, Fri May 16 01:08:36 2003, linker 6.0, entry 0x15742 | 41156 | `e25a214ece83ff2f08d42a6f16e8837c` | full COFF | objdump, ndisasm |
| `ULTRA66.MPD` (Win9x) | PDC20262 only | Thu Sep 16 23:50:06 1999, linker 4.20, entry 0x14a2b | 46012 | `23c59ed1a26c487a04a75b25eaa274d2` | full COFF | objdump |
| `ULTRA66.SYS` (NT) | PDC20262 | 1999, same diskette | 46708 | `f702a4a6a15e6529f6410eec2924a4fa` | | objdump (tables only) |
| `FASTTRAK.SYS` (Win2000) | FastTrak, PDC20246/62/63/65/67 | linked 2000-09-12 | 72045 | `c591331c99b05bf0c1abef8bd573ad01` | stripped | objdump |
| `FASTTRAK.SYS` (NT4, FastTrak100 TX4) | adds PDC20270 | | 94976 | `ebac2d5223bc93d31d1f62d833eb8c54` | 541 symbols | objdump |
| `fasttrak.sys` (WinXP, TX2000) | FastTrak, 9 types incl. PDC20271 | 2.00 build 33, Wed 23 Apr 2003 00:01:11 UTC, entry 0x1d216; PDB `C:\DDK\XP\src\storage\miniport\fasttrak\V2.00B33\sys\i386\fasttrak.pdb` | 75392 | `b3076b8cfca003d561889824d1714e59` | stripped (7) | objdump, rabin2 |
| `fasttrak.sys` (Win2000, TX2000) | same | 00:01:01 | 81704 | `7b43715fd4e66f3f4de4ad06e1407598` | stripped (7) | objdump, rabin2 |
| `fasttrak.sys` (WinNet/2003, TX2000) | same | 00:01:28; PDB under `C:\DDK\3763_NET\...\V2.00B33\...` | 65536 | `e8e5e2d9ece4679c47e958ef35675411` | stripped (7) | objdump, rabin2 |
| `fasttrak.sys` (NT4, TX2000) | same | 00:03:54 | 101200 | `5668249dfa3a2738410ec80112109dc7` | 572 symbols (371 `.text`, 153 `.data` excluding `$SG` literals) | objdump, rabin2 |

md5 values for the ROMs and for `ULTRA66.MPD`, `ULTRA66.SYS`, `ultra.mpd` and
the two FastTrak100-era `FASTTRAK.SYS` files were taken from the files
themselves; the rest are as recorded in the per-binary notes.

Also examined, with no hardware interface found: `PU66VSD.VXD` (1999;
`SMARTVSD_DDB`, "%DOS386 SMARTVSD Device (Version 4.0)"), `PU66VSD.VXD` /
`SMARTVSD.VXD` (2003; `PTIPOWER_DDB` "Promise Technology Inc. ACPI accommodate
driver" FileVersion 1,0,2,22, and `SMARTVSD_DDB` FileVersion 1,0,2,23), both
IOS SCSI-layer shims whose LE object pages were not disassembled [?: they do
not touch the chip]; `PTISTP.DLL`, a user-mode RunDll32 setup helper; and the
INFs `ULTRA.INF`, `ULTRA66.INF`, `FASTTRAK.INF`. `PTIFLASH.EXE` is the DOS
flashing utility; the ROMs contain no flash code of their own.

### 2.3 Other references

- Linux: `pata_pdc202xx_old.c`, `pdc202xx_old.c` / `pdc202xx_old_ide.c`,
  `pdc202xx_new.c`, `pata_pdc2027x.c`.
- CORSAC: `idepdc202xx.cor` (routines `Prepare()`, `InputClock()`,
  `Indexed()`, `SetIndexed()`, `BeforeDma`, `AfterDma`, `UdmaMask()`; tables
  `OldPio`, `OldMwA/B`, `OldUdmaA/B`, `NewPioC/D/13`, `NewMwE/F`,
  `NewUdma10/11/12`).
- dmraid `pdc.c` is the natural cross-check for the RAID metadata (not
  consulted here).
- The emulation model: 86Box `src/disk/hdc_ide_pdc202xx.c` (PDC20269 and
  PDC20271).

---

## 3. The family

### 3.1 Parts and IDs

All parts are vendor 0x105A. [C][L][W]

| Device ID | Part | Products | Generation | Top UDMA (mask of modes 0-6) [C] |
|---|---|---|---|---|
| 0x4D33 | PDC20246 | Ultra33, FastTrak (33) | old | UDMA2, 0x07 |
| 0x4D38 | PDC20262 | Ultra66, FastTrak66 | old | UDMA4, 0x1F |
| 0x0D38 | PDC20263 | FastTrak66 variant | old | UDMA4, 0x1F |
| 0x0D30 | PDC20265 | Ultra100, FastTrak100 | old | UDMA5, 0x3F |
| 0x4D30 | PDC20267 | Ultra100, FastTrak100 | old | UDMA5, 0x3F |
| 0x4D68 | PDC20268 | Ultra100 TX2 | new (TX2) | UDMA5, 0x3F |
| 0x4D69 | PDC20269 | Ultra133 TX2 | new (TX2) | UDMA6, 0x7F |
| 0x6268 | PDC20270 | FastTrak100 TX2 / TX4 / LP | new (TX2) | UDMA5, 0x3F |
| 0x6269 | PDC20271 | FastTrak TX2000 | new (TX2) | UDMA6, 0x7F |
| 0x1275 | PDC20275 | Ultra133 TX2 (mobile / on-board) | new (TX2) | UDMA6, 0x7F |
| 0x5275 | PDC20276 | FastTrak (133) | new (TX2) | UDMA6, 0x7F |
| 0x7275 | PDC20277 | FastTrak (133) | new (TX2) | UDMA6, 0x7F |

The division that matters is not the product name but how the part is told
about transfer timing. The **old** parts keep timing in PCI configuration
space (0x60-0x6F) and switch a 66 MHz clock per transfer; the **new** ("TX2")
parts keep timing behind an index/data pair inside the bus-master block and
clock the ATA bus from a PLL. [L][C][R][W]

A FastTrak and an Ultra of the same generation are the same silicon; only the
class code, the subsystem ID and the option ROM differ (section 11). [R][W]

### 3.2 Generations as Promise's drivers number them

`ultra.sys` / `ultra.mpd` 2.00.43 write a generation code to device-extension
byte `+0xB74` in `_CheckControllerTypeStrict@8`; everything else in the
driver branches on `< 4` (old) or `>= 4` (TX2). [W]

| | gen 1 | gen 2 | gen 3 | gen 4 | gen 5 |
|---|---|---|---|---|---|
| part | PDC20246 | PDC20262 | PDC20265, PDC20267 | PDC20268 | PDC20269, PDC20275 |
| product | Ultra33 | Ultra66 | Ultra100 | Ultra100 TX2 | Ultra133 TX2 |
| device ID | 4D33 | 4D38 | 0D30, 4D30 | 4D68 | 4D69, 1275 |
| **class:subclass required** | **01:04** | 01:80 | 01:80 | 01:80 | 01:80 |
| top UDMA | 2 | 4 | 5 | 5 | 6 |
| timing lives in | config 0x60 | config 0x60 | config 0x60 | indexed 0x0C | indexed 0x0C |
| cable detect | not used | cfg 0x50 b10/11 | cfg 0x50 b10/11 | indexed 0x0B b2 | indexed 0x0B b2 |
| 66 MHz clock at BAR4+0x11 | no | yes | yes | no | no |
| index/data pair | no | no | no | yes | yes |
| PLL target | none | none | none | 100 MHz | 133 MHz |
| BAR5 memory window used | no | no | no | no | **yes, 16 KiB** |
| BAR4+0x1C, +0x1F strap fix-ups | no | no | **yes** | no | no |

The FastTrak drivers number the same silicon differently: `fasttrak.sys` 2.00
build 33 keeps a chip type in `_FastTrakController` (0x1e608) and branches
on `>= 6` (TX2, indexed registers) and `>= 7` (UDMA133 part). [W]

| `_FastTrakController` | `ultra.sys` gen | Device IDs | Part |
|---|---|---|---|
| 1 | 1 | 4D33 | PDC20246 |
| 2, 3 | 2 | 4D38, 0D38 | PDC20262, PDC20263 |
| 4, 5 | 3 | 4D30, 0D30 | PDC20267, PDC20265 |
| 6 | 4 | 6268 | PDC20270 |
| 7, 8, **9** | 5 | 5275, 7275, **6269** | PDC20276, PDC20277, **PDC20271** |

The PDC20271 sits on the far side of both thresholds: it is a UDMA133 TX2
part, i.e. a PDC20269 with a RAID class code. [W]

### 3.3 Things a model that treats the family as one part gets wrong

- **The Ultra33 reports itself as a RAID controller.** Class 01:04, where every
  later Ultra part is 01:80 and a plain IDE controller would be 01:01.
  `ultra.sys` tests this per device ID, so a PDC20246 answering 01:80 is
  rejected as firmly as a PDC20262 answering 01:04. Neither Linux nor CORSAC
  reads the class code. [W]
- **The 66 MHz clock bit exists only on the old parts.** It appears with the
  Ultra66 and is gone by the TX2 generation, which uses the PLL. [L][W]
- **The indexed register file and the PLL arrive together** with the TX2
  parts; everything the old parts kept in configuration space, cable detect
  included, moves behind them. [L][R][W]
- **Only the Ultra133 TX2 generation has a memory window that anything uses.**
  `ultra.sys` maps BAR5 on generation 5 alone (section 4.4). [W]
- **The two TX2 speeds want different PLL frequencies**, 100 MHz for the
  PDC20268 and 133 MHz for the PDC20269; the option ROM decides by whether any
  attached drive reports UDMA6 (section 6.4). [R]
- **Generation 3 has two strap-fed registers nobody else has**, BAR4+0x1C and
  BAR4+0x1F bit 7 (section 5.2). [W]
- **Retail cards are bound by subsystem ID.** The Ultra66 ROM refuses a card
  whose subsystem device is not 0x4D33, and `ULTRA.INF` claims the non-TX2
  Ultra parts only with `SUBSYS_4D33105A`. On-board implementations fall to
  the generic IDE driver. [R][W]

### 3.4 Option ROM sizes

| Card | ROM size | PCIR class code bytes (prog-if, subclass, class) |
|---|---|---|
| Ultra66 (`Ul200b18.BIN`) | 16 KiB (`55 AA 20`); shrinks itself at run time to as little as 8 KiB | `8F 80 01` |
| Ultra133 TX2 / Ultra100 TX2 | 16 KiB | `8F 80 01` |
| FastTrak100 (`ft100B24.bin`) | 64 KiB (`55 AA 80`) | `D8 04 01` |
| FastTrak TX2000 (`bios.bin`) | 64 KiB (`55 AA 80`) | `8F 04 01` |

[R] (See section 14 for a note on the Ultra133 TX2 prog-if byte.)

---

## 4. PCI configuration space

### 4.1 Header

| Offset | Value | Notes |
|---|---|---|
| 0x00 | 0x105A | Vendor. |
| 0x02 | device ID | Section 3.1. |
| 0x04 | command | Every ROM sets bit 2 (bus master) with a read-OR-write and assumes the system BIOS set I/O enable. [R] The FastTrak100 Win2000 driver writes 0x0007 (I/O, memory, bus master); `fasttrak.sys` 2.00 b33 never writes it. [W] |
| 0x08 | revision | The Ultra66 ROM 2.00.18 considers UDMA5 only when the revision is >= 2 (`Ul200b18.BIN 0x2961`); v1.14 does not read it. [R] |
| 0x09 | prog-if | 0x8F in the Ultra PCIRs: native on both channels, programmable, bus master. Nothing reads it. [R] |
| 0x0A-0x0B | class/subclass | 0x0180 on Ultra parts except the PDC20246 (0x0104); 0x0104 on every FastTrak. Enforced by the ROMs and drivers (sections 9, 10). [R][W] |
| 0x10-0x1C | BAR0-BAR3 | I/O, bit 0 set; the Ultra ROMs refuse the card if BAR0/BAR2 lack bit 0 (section 9.2). |
| 0x20 | BAR4 | I/O; bus master and Promise registers (section 5). |
| 0x24 | BAR5 | Memory, 16 KiB, PDC20269/20275 (section 4.4). |
| 0x2C-0x2F | subsystem | 105A:4D33 on the retail Ultra33/66/100; 105A:4D32 demanded by the FastTrak100 ROM; 105A:4D39 in `FASTTRAK.INF` (section 11.4). |
| 0x30 | expansion ROM BAR | Sized 16 KiB (Ultra) or 64 KiB (FastTrak) (section 4.5). |
| 0x3C | interrupt line | Read by every ROM to choose the IRQ vector; written back by the FastTrak ROMs after `B10F` (section 9.5). |

### 4.2 Base address registers

| BAR | Contents | Accessed as |
|---|---|---|
| 0 | primary task file, 8 ports | `& 0xFFFE` (drivers), `& ~3` (Ultra ROMs), `& ~1` (FastTrak ROMs) |
| 1 | primary control, 4 ports; the drivers use `(BAR1 & 0xFFFC) \| 2`, the Ultra ROMs `BAR1 + 1` on the raw value (bit 0 set), both giving base+2 | |
| 2 | secondary task file, 8 ports | |
| 3 | secondary control, 4 ports | |
| 4 | bus master and Promise registers | Section 5. |
| 5 | 16 KiB memory window, PDC20269/20275 | `& 0xF0` (`ultra.sys`, `ultra.mpd`) |

[L][R][W]

Width of BAR4 as the drivers claim it: `fasttrak.sys` 2.00 b33 registers
BAR4 with `RangeLength` 0x20 on type 1, **0x40** on types 2-5 (covering
+0x20/+0x24) and **0x10** on types >= 6 (only the two bus-master/indexed
blocks) (`0x19e99`, `0x19ea5`, `0x19eb1`). [W] The Win9x analysis puts the old
parts' BAR4 at "32 bytes at minimum, and really 0x28", since +0x11, +0x1A-0x1F,
+0x20 and +0x24 are all touched. [W] The 86Box model decodes 64 bytes. [E]

### 4.3 Promise configuration registers

| Offset | Width | Who | Behaviour |
|---|---|---|---|
| 0x40 | word | Ultra133 TX2 ROM (`U133B15.BIN 0x2B24`), TX2000 ROM (`bios.bin 0x05A9`), `fasttrak.sys` 2.00 b33 on types >= 6 (`0x19091`) | **Written 0x0000**, never read back. [R][W] Linux sets only bit 0 of byte 0x40, only on Apple "Kiwi" boards. [L] Meaning unknown. |
| 0x42 | byte | `ultra.sys` / `ultra.mpd` 2.00.43, every adapter, both probe paths (`ultra.sys 0x11e79`, `0x12017`; `ultra.mpd 0x11fc3`, `0x12161`) | Read, `&= 0xFE`, written back: **bit 0 cleared**. Never read otherwise. Not in the 1999 driver, Linux, CORSAC or `fasttrak.sys`. [W] [?: 0x40-0x43 may be one Promise-private dword whose layout changed between generations.] |
| 0x50 | word | old parts | Bits 10 (ch0) and 11 (ch1): cable, **set = 40-conductor**. Bit 6: an undocumented strap, copied into BAR4+0x1F bit 7 on generation 3. [L][C][R][W] Not read by the TX2 ROMs or by `fasttrak.sys` on TX2 parts. |
| 0x60 + 8·ch + 4·drv | 3 bytes (written as a dword by the drivers) | old parts | Per-drive timing, section 7.1. [L][C][R][W] |
| 0x62 / 0x6A | byte | FastTrak ROMs | Bit 6 (0x40): on the FastTrak100 ROM selects the 66 MHz timing column and IDENTIFY word 88 vs 86 (`ft100B24.bin 0x0A48`, `0x097D`); on the TX2000 ROM, on a live path, selects IDENTIFY word 68 vs 67 for the MWDMA cycle time (`bios.bin 0x093C`). [R] |

Both Windows 2.00.43 drivers write the timing block with a **4-byte**
`ScsiPortSetBusDataByOffset` at `0x60 + 4·n` (n = 2·channel + drive),
so the fourth byte (0x63/0x67/0x6B/0x6F) is written as 0x00, and none of the
bytes is read-modify-written. [W] Neither ever writes the latency timer, which
CORSAC's `Prepare()` does. [W][C]

`FASTTRAK.SYS` (Win2000 FT100 and TX2000 builds) reads config word 0x30,
masks it with 0xC000 and stores it in the device extension, never to use it
(`FT100 0x1928F`, `TX2000 0x19614`). Unexplained. [W]

### 4.4 BAR5: the memory window (PDC20269/20275)

`ultra.sys` maps BAR5 on generation 5 only (`_AtapiFindPCIController@24`,
`0x1255f`): `RangeLength = 0x4000`, `RangeInMemory = 1`, then
`ScsiPortGetDeviceBase`. `ultra.mpd` maps it twice, through
`ScsiPortGetDeviceBase` and through VMM `_MapPhysToLinear` (`int 20h`, service
0x0001:0x006C). [W]

| Window offset | Width | Contents |
|---|---|---|
| `0x1000 + 8·ch + 0` | 8 | bus-master command, mirrored; 0x08/0x09/0x00/0x01 written |
| `0x1000 + 8·ch + 4` | 32 | PRD table physical address (`_PreparePRDTable133@8`) |
| `0x17C0` (ch0) / `0x15C0` (ch1) | | base of a memory-mapped task file |
| ... `+0x00` | 16 | data (moved with `ScsiPortWriteRegisterBufferUshort`) |
| ... `+0x0A` | 8 | sector count |
| ... `+0x0F` | 8 | sector number / LBA 7:0 |
| ... `+0x10` | 8 | cylinder low / LBA 15:8 |
| ... `+0x15` | 8 | cylinder high / LBA 23:16 |
| ... `+0x1A` | 8 | device/head: `((drive & 1) \| 0xFA) << 4` = 0xA0/0xB0, `\| LBA 27:24` |
| ... `+0x1F` | 8 | command on write (0x20/0x24/0x25/0x29/0x30/0x34/0x35/0x39/0xC4/0xC5/0xC8/0xCA), status on read |
| `0x1FD8` (ch0) / `0x1DD8` (ch1) `+0x02` | 8 | alternate status |

[W] Channel 1's blocks sit 0x200 **below** channel 0's. The irregular stride
(0x0A, 0x0F, 0x10, 0x15, 0x1A, 0x1F) is read from the raw encodings
(`8d 43 0a`, `8d 43 0f`, `8d 43 10`, `8d 43 15`, ...), and LBA48 is issued as
two passes over the same five offsets, high-order bytes first, so they are
certainly the task-file registers. [W]

Only `_IdeReadWrite133@8` and `_PreparePRDTable133@8` use the window, selected
in `_IdeSendCommand@8` by `cmpb $0x5,0xb74`. IDENTIFY, ATAPI, SMART, verify,
reset and media status all still use the I/O task file. The memory window
must alias the same task-file state as the I/O ports, because the driver
issues IDENTIFY through the ports and the next READ through memory. [W]

Nothing reaches BAR5 in: Linux `pdc202xx_new.c` (port I/O only), CORSAC,
either Ultra TX2 option ROM, the TX2000 option ROM, or any build of
`fasttrak.sys` 2.00 b33, which imports no `ScsiPort*Register*` accessor
(`rabin2 -i`), never reads config 0x24, and declares five access ranges. [L][C][R][W]
`pata_pdc2027x.c` does use a BAR5 window on the PDC20268/20269, but only for
timing and PLL registers at low offsets (`PDC_PLL_CTL` at 0x1202, test mode in
the dword at 0x1100) and never for a task file or bus master. [L]

Whether the PDC20268 has a BAR5 at all is unresolved: `ultra.sys` never maps it
on generation 4; `pata_pdc2027x` suggests it exists. [W][L]

### 4.5 Expansion ROM BAR

The option ROM BAR must size correctly: the bits below the ROM size must be
read-only zero, or firmware that writes all ones reads its own ones back,
concludes the card wants 4 GiB, and declines to place the ROM, which looks
exactly like a card with no ROM. 16 KiB on the Ultra cards, 64 KiB on the
FastTrak cards. [E]

---

## 5. The bus-master block and the Promise registers

Offsets are from BAR4. The first 16 bytes are a standard SFF-8038i bus master,
primary at +0x00 and secondary at +0x08; everything above is Promise's. [L]

### 5.1 Register map

| Offset | Width | Parts | Purpose | Sources |
|---|---|---|---|---|
| +0x00 / +0x08 | 8 | all | SFF bus-master command | [L][R][W] |
| +0x01, +0x03 / +0x09, +0x0B | 8 | **TX2 only** | **index / data pair** (section 5.3); in the bytes SFF-8038i leaves reserved | [L][C][R][W] |
| +0x02 / +0x0A | 8 | all | SFF bus-master status; bit 2 = interrupt, write 0x04 to clear | [L][R][W] |
| +0x04 / +0x0C | 32 | all | PRD table address | [L][R][W] |
| +0x11 | 8 | gens 2-3 | clock select: 0x02 = ch0, 0x08 = ch1 | [L][C][R][W] |
| +0x1A / +0x1B | 8 | old | per-channel "mode" register; written 0x01 | [R][W] |
| +0x1C | 8 | gen 3 | low bits forced to 3 | [R][W] |
| +0x1D | 8 | old | interrupt and FIFO status | [L][R][W] |
| +0x1E | 8 | FastTrak100 ROM | written 0x00 once at probe | [R] |
| +0x1F | 8 | old | bit 0 "UDMA speed flag"; bit 4 reset; bit 7 strap copy | [L][R][W] |
| +0x20 / +0x24 | 32 | old | transfer length / direction assist, ch0 / ch1 | [L][R][W] |

### 5.2 Old-generation registers in detail

**+0x11, clock select.** One bit per channel, 0x02 primary and 0x08 secondary.
[L] Linux, CORSAC (`BeforeDma`/`AfterDma`), `ultra.sys` and `fasttrak.sys` set
it immediately before starting a transfer faster than UDMA2 and clear it when
the transfer ends (drivers clear it in the ISR), with a read-modify-write.
[L][C][W] The Ultra66 ROM writes it **bare** (0x02 or 0x08 before every DMA
transfer, 0x00 after; `Ul200b18.BIN 0x0391/0x03F2, 0x0875/0x08D6`), clobbering
the other channel's bit, and enables it for every transfer regardless of mode.
[R] The part does not latch a speed once; it is switched per transfer. This is
why UDMA3-5 reuse the timing values of UDMA1-2 (section 7.1). `fasttrak.sys`
uses it on types 2-5 only. [W]

**+0x1A / +0x1B.** The Ultra66 ROM writes 1 immediately before starting a
transfer and 0 after (`Ul200b18.BIN 0x03BF/0x03EB`); `ultra.sys`, `ultra.mpd`,
`ULTRA66.MPD` and the FastTrak100 ROM write 0x01 once at init. [R][W] Linux only
reads them ("primary_mode", "secondary_mode"). [L] Nothing ever reads them back
in the vendor code. `fasttrak.sys` 2.00 b33 and the TX2000 ROM never touch
them, so they are not required for operation. [W][R] [?: "channel armed".]

**+0x1C.** Generation 3 only: `ultra.sys`/`ultra.mpd` read, `|= 0x03`, write
back (`ultra.sys 0x11951`); the FastTrak100 ROM reads, `& 0xF0`, `| 0x03`,
writes (`ft100B24.bin 0x05BC-0x05C8`). [W][R] [?: "enable both channels".]

**+0x1D, interrupt and FIFO status.** Four bits per channel: [L][R]

| Bit, primary | Bit, secondary | Meaning |
|---|---|---|
| 0 | 4 | FIFO empty |
| 1 | 5 | FIFO full |
| 2 | 6 | this channel is interrupting |
| 3 | 7 | error |

Linux uses bits 2 and 6 as the shared-interrupt "is it mine" test
(`pdc202xx_irq_check()`). [L] The Ultra66 ROM ends every DMA transfer by
polling this register for **both** the interrupting bit and the FIFO-empty bit
of the channel (`Ul200b18.BIN 0x05EC`), and its IRQ handlers use bits 2/6 to
select the channel (section 8.3). [R] `ULTRA66.MPD` polls bit 0/4 (FIFO empty)
before ATAPI DMA (`0x13bcc`, up to 5000 reads), which confirms Linux's layout
from the vendor side. [W] The FastTrak100 ROM's IRQ handler reads bits 2/6
(`ft100B24.bin 0x2335-0x2348`). [R] Bits 1/3/5/7 are never tested by any ROM.
Neither 2003 Windows driver nor `fasttrak.sys` 2.00 b33 reads +0x1D at all.
[W] Whether it exists on the TX2 parts is untested.

**+0x1F.** [L][R][W]

- Bit 0: Linux's "UDMA speed flag" (`burst = ioread8(bmdma+0x1f); iowrite8(burst|0x01, ...)`).
  The Ultra66 ROM writes a literal 0x01 once during enumeration
  (`Ul200b18.BIN 0x217A`); v1.14 does the same (0x1B1A). `ultra.sys` writes
  0x01 at init on gens 1-3. The FastTrak100 ROM read-ORs 0x01. Never read back.
- Bit 4: chip reset. `ultra.sys` `_AtapiResetController@8` sets it, waits
  50 ms, clears it (RMW). `fasttrak.sys` `_ResetController@4` writes 0x10,
  waits 100 ms, writes 0x01, waits 300 ms (whole-byte writes; bit 7 not
  preserved). Not in Linux or CORSAC.
- Bits 7:6: on generation 3 `ultra.sys` masks the register to 0x3F and sets
  bit 7 iff configuration word 0x50 bit 6 is set (`0x11968`); the FastTrak100
  ROM ORs 0x80 under the same condition (`ft100B24.bin 0x05DA`). Meaning
  unknown; a strap of some kind.

**+0x20 + 4·ch, transfer length assist.** A 32-bit register written before
starting DMA for the cases the part's DMA engine "will not complete correctly
without help" (Linux's words): LBA48 and ATAPI DMA. [L] Field structure, from
the vendor drivers: [W]

| Bits | Meaning |
|---|---|
| 23:0 | transfer length in 16-bit words (bytes >> 1) |
| 24 | read (device to host) |
| 25 | write (host to device) |
| 26 | UltraDMA; set by both vendor drivers only when the drive is actually in UltraDMA |

Linux writes `0x05000000` for a read and `0x06000000` for a write, i.e. the
same direction bits with bit 26 always set; CORSAC copies that. [L][C]
`fasttrak.sys` 2.00 b33 (`_IdeReadWrite@20`, separate read path `0x1af2d` and
write path `0x1b032`) sets bit 24 on reads and bit 25 on writes, forming the
length as sector count << 8 plus direction; `ultra.sys` (`0x13d83`) agrees.
The register is cleared to 0 after the transfer (ISR, and
`_StopPromiseAtapiDMACommand@8`). [W] See section 14 for the conflicting
readings of `ultra.mpd` and the FastTrak100 ROM.

Who writes it:

- `ultra.sys` / `ultra.mpd`: ATAPI DMA on gens 1-3; LBA48 DMA on gen 3 only
  (flag bit 0x4000, set only under `cmpb $0x3,0xb74`). Never on gens 4/5. [W]
- `fasttrak.sys` 2.00 b33: every DMA read and write above UDMA2 on types
  2-5, i.e. when the drive record's cached IDENTIFY-derived word at `+0xE6` has
  bits 0x4000 and 0x0400 both set (`0x18a00-0x18a12`). Never on the TX2 family,
  where the chip evidently derives the length itself. [W]
- The FastTrak100 ROM, for ATAPI/DMA (`ft100B24.bin 0x2B16-0x2B25`,
  `0x2B70-0x2B82`), zeroed on stop. [R]

### 5.3 The TX2 index/data pair

| Port | Role |
|---|---|
| BAR4 + 8·ch + 1 | index |
| BAR4 + 8·ch + 3 | data |

So the primary pair is at +0x01/+0x03 and the secondary at +0x09/+0x0B. Both
8-bit. To write: index then data; to read: index then read data. The index is
**not** auto-incrementing: every burst rewrites it before each data byte. The
ROMs put two `jmp $+2` delays on each side; `fasttrak.sys` stalls 1 µs between
index and data at probe, 10 µs on resume. [L][C][R][W]

This matches `get_indexed_reg`/`set_indexed_reg` in `pdc202xx_new.c`
(`dma_base + 1` / `dma_base + 3`) and CORSAC's `Indexed()`/`SetIndexed()`. [L][C]
The pair sits in the SFF block's reserved bytes, so an emulation must decode
it before the bus-master core sees the access, and only on TX2 parts. [L]

### 5.4 The indexed register file

Per-drive registers take +0x08 for the slave. [L][C][R][W]

| Index | Access | Meaning | Sources |
|---|---|---|---|
| 0x00 | W | Written 0 by the TX2000 ROM right after BAR4 is read (`bios.bin 0x045C`, `out BAR4+1, 0x00`, no data access follows). Inert. | [R] |
| 0x01 | RMW | Control. **Bit 6 (0x40): PLL counter test mode.** **Bits 3:2 (0x0C): channel reset** (cleared, then set). | [L][C][R][W] |
| 0x02 | R/W | PLL feedback divider F. **Chip-global, used only through the secondary pair.** | [L][R][W] |
| 0x03 | R/W | PLL reference divider R. Secondary pair only. | [L][R][W] |
| 0x0B | R | Bit 2: cable, **set = 40-conductor**. Bits 7 and 5: status (see below). | [L][C][R][W] |
| 0x0C, 0x0D | W | PIO timing | [L][C][R][W] |
| 0x0E, 0x0F | W | MWDMA timing | [L][C][R][W] |
| 0x10, 0x11, 0x12 | W | UDMA timing; 0x10 bit 7 is cleared by firmware and drivers (below) | [L][C][R][W] |
| 0x13 | W | PIO timing; bit 1 set by the vendor drivers for ATA disks (below) | [L][C][W] |
| 0x14-0x1B | W | the same eight registers for the slave | [L][C][R][W] |
| 0x20 | R | PLL counter, low byte (section 6.1) | [L][R] |
| 0x21 | R | PLL counter, high part (7 bits) | [L][R] |

No other index value is written by any ROM examined. [R]

**Index 0x01, bits 3:2, channel reset.** `ultra.sys` `_AtapiResetController@8`
(`0x1088d`): `&= 0xF3`, 1000 µs, `|= 0x0C`. `fasttrak.sys` `_ResetController@4`
(`0x1c042`): the same bits with a 150 ms hold and a 300 ms settle. A model must
survive this without losing the drives. [W]

**Index 0x0B, bits 7 and 5.** `ultra.sys` `_AtapiTimer@4` (`0x14410`) reads
index 0x0B on both pairs when an SRB times out and treats
`(value & 0xA0) == 0x20` as a wedged DMA engine, whereupon it force-stops the
bus master (polls command bit 0 up to 0x5000 times, then writes 0). [W]
[?: by analogy with the old family's 0x1D, 0x20 is a FIFO-full indication and
0x80 an error latch.] The Ultra133 TX2 ROM reads bit 5 only for a slave ATAPI
device (`U133B15.BIN 0x3158`/`0x3163`) and, if set, re-selects the master
(0xA0) and reads its status once; returning 0 is safe. The TX2000 ROM and
`fasttrak.sys` never read 0x0B after the cable check. [R][W] **Safe model:
bit 2 per cable, everything else zero.**

**Index 0x10/0x18, bit 7.** The Ultra133 TX2 ROM clears it (`and 0x7F`)
unconditionally for every drive (`U133B15.BIN 0x2924`); `ultra.sys`
(`0x11848-0x118bf`), `ultra.mpd` (`0x1199f-0x11a16`) and `fasttrak.sys`
(`0x19096-0x1912f`) clear it for all four drives at init. Linux and CORSAC do
it only for UDMA2 on the 100 MHz parts. Every value in the UDMA table already
has bit 7 clear, so this matters only for the value the firmware left. The
TX2000 ROM contains the same clear but never executes it (section 11.3).
[R][W][L][C] [?: a hold-time or extra-wait bit (tHOLD).]

**Index 0x13, bit 1.** Set by `_SetPdc20269Speed@8` after merging the tables
(`ultra.sys 0x10d88`, `ultra.mpd 0x10eee`) for ATA devices, not for ATAPI; set
unconditionally by `fasttrak.sys` (`0x18fba`), which attaches only disks. No
table value has it set. [W] [?: the TX2 FIFO/prefetch enable, by analogy with
bit 4 of the old family's PIO byte A.] See section 14 on the polarity.

**Index space vs `pata_pdc2027x`'s MMIO view.** Index 0x01 bit 6 corresponds to
bit 14 of the dword at MMIO 0x1100, and indices 0x20/0x21 to 0x1120, which
suggests "indexed register N is byte N of the MMIO block at 0x1100"; but
`pata_pdc2027x` places `PDC_PLL_CTL` at 0x1202, not 0x1102. Either the index
space is not a flat alias or 0x1102 and 0x1202 alias in hardware. For port-I/O
emulation the indexed view is authoritative. [L][R]

---

## 6. The PLL (TX2 generation)

The TX2 parts clock the ATA bus from a PLL whose input is derived from the PCI
clock. Firmware does not read the input frequency; it **measures** it with a
30-bit down-counter, then computes the PLL dividers. Return the wrong count and
the firmware computes an impossible multiplier, or divides by zero.

### 6.1 The counter

A 30-bit counter that **decrements**, read in four pieces through the indexed
registers of both channels: [L][R]

| Piece | Index | Pair | Bits |
|---|---|---|---|
| cnt0 | 0x20 | primary | 7:0 |
| cnt1 | 0x21 | primary | 14:8 (7 bits; bit 7 must read 0) |
| cnt2 | 0x20 | secondary | 22:15 |
| cnt3 | 0x21 | secondary | 29:23 (7 bits) |

```
count = (cnt3 << 23) | (cnt2 << 15) | (cnt1 << 8) | cnt0
```

The shifts are 8, 15, 23, not 8, 16, 24: `pdc202xx_new.c::read_counter()`.
The ROMs assemble it as `((sec21<<8 | sec20) << 15) | (pri21<<8 | pri20)` and do
**not** mask the primary half to 15 bits, so bit 7 of primary index 0x21 must
read 0. [L][R]

Test mode is indexed 0x01 bit 6 (0x40), set and cleared with read-modify-write.
[L][R]

### 6.2 Measurement: Linux

The driver reads the counter, enables test mode, waits 10 ms, reads again,
disables test mode, and computes [L]

```
pll_input = ((start - end) & 0x3FFFFFFF) / 10 * (10000000 / usec_elapsed)
```

It reads the counter up to three times and rejects a reading when the top bits
changed between reads or when the count went up. It rejects an input below
5 MHz or above 70 MHz. [L]

### 6.3 Measurement: the option ROMs

`U133B15.BIN 0x2C65` (called from 0x2ED7, immediately after the ATA soft
resets) and `bios.bin 0x0FEA` (TX2000, during PCI enumeration, first adapter
only, guarded by `[cs:0x3F5] == 0`): [R]

1. Arm a timeout of **36 BIOS ticks** (`mov ax,0x24`) from `0040:006C`.
2. Index 0x01 on the **primary** pair: read, `or 0x40`, write. START.
3. Spin until the tick count is reached: 36 × 54.9254 ms = **1.9773 s**, at
   most one tick more (2.0322 s).
4. Index 0x01: read, `and 0xBF`, write. STOP.
5. **Only now** read the counter: 0x21 then 0x20 on the primary pair, then
   (`add dx,6`, BAR4+3 to BAR4+9) 0x21 then 0x20 on the secondary pair.
6. `elapsed = ~C & 0x3FFFFFFF`.
7. `[clk] = elapsed / 200000` (built as `100000 × 2`, 32-bit `div ecx`),
   stored at `U133B15.BIN [cs:0x3D4]` / `bios.bin [cs:0x3F5]`.

Two consequences an emulation must satisfy:

- **The ROM takes no baseline sample.** It assumes the counter stood at
  **0x3FFFFFFF** at the moment test mode was entered. [R]
- **It reads the counter after clearing test mode**, so a stopped counter must
  **hold** its value; one that snapped back to the top when the bit fell would
  report zero elapsed time. [R][E]

Because the interval is about 2 s, `elapsed ≈ 2·f_in` and `[clk] ≈ f_in / 100000`,
the PLL input in units of 100 kHz: 166 for a 16.667 MHz input (half a 33.33 MHz
PCI clock). [R]

**Divide-by-zero hazard.** The programming step divides a 16-bit constant by
`[clk]` with a 16-bit `div bx` (`U133B15.BIN 0x2823`, `bios.bin 0x0FAC`). If the
counter delta is below 200 000, `[clk]` is 0 and the ROM takes a divide-error
exception at POST and the machine stops. If `[clk]` is so small that
`15000/[clk]` exceeds 255 nothing faults (only AL is used) but F is nonsense.
**Keep `[clk]` roughly within 50-250, an apparent input of 5-25 MHz.** The ROMs
perform no sanity check on the measured clock, unlike Linux. [R]

### 6.4 The 100 / 133 MHz decision

`[cs:0x3D3]` (U133) / `[cs:0x3FB]` (TX2000) starts at 0 and is set to 1 as soon
as **any attached ATA drive reports UDMA mode 6** in IDENTIFY word 88 (bit 6),
with word 53 bit 2 set (`U133B15.BIN 0x2FC6-0x2FD8`; `bios.bin 0x086D-0x0877`,
which also requires word 88 bits 2:0 non-zero). That one bit decides: [R]

1. the PLL target (133 MHz vs 100 MHz);
2. whether the explicit timing registers 0x0C-0x1B are written at all
   (`U133B15.BIN 0x2905`, `0x36A3`; `bios.bin 0x17B2`).

This matches the comment in both Linux drivers: at 100 MHz the ASIC sets its
timing registers itself in response to SET FEATURES; at 133 MHz the driver must
write them. [L][R] **An emulation that never reports UDMA6 will see the ROM leave
the chip at 100 MHz and never write indexed 0x0C-0x1B.**

The Ultra100 TX2 ROM (`U100B15.BIN`, `B14.BIN`) has no UDMA6 flag: the byte at
0x3D3 holds the measured clock instead, the constant is always 15000 and R is
always 0x0D (`U100B15.BIN 0x27C4-0x27F1`). [R]

### 6.5 Programming

The output of the PLL is

```
POUT = (F + 2) / ((R + 2) · NO)          (times the input clock)
```

with F in indexed 0x02 and R in indexed 0x03, both written through the
**secondary** channel's pair (BAR4+9 / BAR4+0x0B), on every controller found,
with the value measured on controller 0's primary pair. [L][R][W]

**Option ROMs** (`U133B15.BIN 0x2809`, `bios.bin 0x0F88`): [R]

| Flag | F | R | Target |
|---|---|---|---|
| 0 | `15000 / [clk] − 2` | 0x0D | 100 MHz |
| 1 | `13300 / [clk] − 2` | 0x08 | 133 MHz |

**Linux** forms `ratio = pll_output / (pll_input / 1000)` with a target of
100 MHz (Ultra100 TX2) or 133.333 MHz (Ultra133 TX2), and chooses R: [L]

| Ratio | R | NO |
|---|---|---|
| < 8600 | 0x0D | 1 |
| < 12900 | 0x08 | 1 |
| < 16100 | 0x06 | 1 |
| < 64000 | 0x00 | 1 |
| otherwise | give up | |

then `F = (ratio × (R + 2)) / 1000 − 2`, rejecting F outside 0-127. [L]

Cross-check:

- 100 MHz, R = 0x0D (R+2 = 15): Linux `F = 1500000 / pll_input_kHz − 2`; ROM
  `15000 / (pll_input_kHz / 100) − 2`, identical. [L][R]
- 133 MHz: the ROM hard-codes R = 8 and uses 13300, i.e. exactly 133.0 MHz,
  0.25 % low; Linux for a 16.667 MHz input computes `ratio = 8000 < 8600`,
  R = 0x0D, F = 118. The ROM gets R = 8, F = 78. Both satisfy
  `(F+2)/(R+2) = 8`. **An emulated PLL must accept either pair.** [L][R]

Worked values: `f_in = 16 666 667 Hz`, `T = 1.9773 s` gives `elapsed = 32 955 000`,
`[clk] = 164`, `F = 13300/164 − 2 = 79`, `R = 8`, `POUT = 81/10 × 16.667 = 135 MHz`.
At exactly `T = 2.000 s`: `[clk] = 166`, `F = 78`, `POUT = 133.33 MHz`. [R]

### 6.6 Who measures, who preserves

- **Only the option ROM ever computes F and R.** Neither Windows driver
  measures the PLL: `ultra.sys`, `ultra.mpd` and `fasttrak.sys` never write
  index 0x01 bit 6 or read 0x20/0x21. [W]
- `ultra.sys` / `ultra.mpd` read 0x02/0x03 through the secondary pair at probe
  into `ext+0xA96/0xA97` (`ultra.sys 0x123f2-0x1242c`) and write them back in
  `_AtapiInitDevice@4` after every `HwInitialize`, i.e. after every bus reset
  (`ultra.sys 0x118e0-0x11907`). [W]
- `fasttrak.sys` reads them at probe into `ext+0x25/0x26` (`0x19f7d-0x1a032`) and
  writes them back **only on power resume**, `_RaidIwasWakedup@4` (`0x1b504`),
  **not** after its own chip reset. [W]
- CORSAC's `Prepare()`/`InputClock()` measure and recompute like Linux. [C]

So the PLL registers **must survive a chip or channel reset** and must read
back what was written. On a machine that boots with the card's ROM enabled, the
PLL is programmed once by firmware and merely preserved thereafter. [W][E]

---

## 7. Transfer timing

**The timing tables are byte-for-byte identical** across the option ROMs of
every build examined, the 1999 and 2003 Ultra drivers in every OS build, the
FastTrak drivers, Linux, and CORSAC. For the TX2 tables the 80-byte ROM block
`U133B15.BIN 0x2398-0x23E7` equals `bios.bin 0x0398-0x03E7` (compared
directly); the 0xBC-byte `.data` range of `fasttrak.sys` equals `ultra.sys`
`0x15AD8..0x15B50` in all its non-zero bytes. The Ultra100 TX2 ROMs carry the
same tables (at `0x2348`...`0x237C`) but never use them. [R][W][L][C]

### 7.1 Old parts: configuration space 0x60 + 8·ch + 4·drv

Three bytes per drive, A, B, C (a fourth byte, D, is written as 0 by the vendor
drivers): [L][C]

| Byte | Bits | Field |
|---|---|---|
| A | 7 | ERRDY_EN (Linux name) |
| A | 6 | SYNC_IN (Linux name) |
| A | 5 | IORDY enable |
| A | 4 | FIFO / prefetch enable |
| A | 3:0 (5:0 per Linux mask) | PIO, first half |
| B | 7:5 | DMA timing, first half |
| B | 4:0 | PIO, second half |
| C | 3:0 | DMA timing, second half |

Linux preserves bits 7:6 of A (`r_ap &= ~0x3F; /* Preserve ERRDY_EN, SYNC_IN */`)
and sets 0x20 when IORDY is needed; CORSAC merges under masks 0x3F / 0x1F /
0xE0 / 0x0F. [L][C] The vendor code **composes** the bytes outright:

- The Ultra66 ROM writes `A = 0xE0 | (0x10 unless ATAPI) | TA`, all three bytes,
  in one pass, ORing the PIO and DMA entries together
  (`Ul200b18.BIN 0x2AAC-0x2B32`). [R]
- `ultra.sys` / `ultra.mpd` write A from `_MoryPIOSpeed` (0xE_ values) when the
  drive supports IORDY, `_MoryPIOSpeed_Nio` (0xC_) otherwise; the two differ by
  exactly 0x20. Bit 0x10 is ORed in for non-ATAPI devices. [W]
- `fasttrak.sys` 2.00 b33 masks A to **bits 1:0 for the slave drive**
  (`0x18ee3: and $0x3,%edx`). [W] [?: bits 7:2 of A are per-channel and decoded
  only from the master's register.]
- The FastTrak100 ROM ORs 0xD0 into A, plus 0x20 when config 0x62/0x6A bit 6
  is set (`ft100B24.bin 0x0A31`, `0x0A4D`). [R]

**PIO** (A low bits, B low bits): [L][C][R][W]

| Mode | A | B | Linux/CORSAC word | `_MoryPIOSpeed` | `_MoryPIOSpeed_Nio` |
|---|---|---|---|---|---|
| 0 | 0x09 | 0x13 | 0x0913 | `E9 13` | `C9 13` |
| 1 | 0x05 | 0x0C | 0x050C | `E5 0C` | `C5 0C` |
| 2 | 0x03 | 0x08 | 0x0308 | `E3 08` | `C3 08` |
| 3 | 0x02 | 0x06 | 0x0206 | `E2 06` | `C2 06` |
| 4 | 0x01 | 0x04 | 0x0104 | `E1 04` | `C1 04` |

**UDMA** (B under mask 0xE0, C under mask 0x0F): [L][C][R][W]

| Mode | B | C |
|---|---|---|
| 0 | 0x60 | 0x03 |
| 1 | 0x40 | 0x02 |
| 2 | 0x20 | 0x01 |
| 3 | 0x40 | 0x02 |
| 4 | 0x20 | 0x01 |
| 5 | 0x20 | 0x01 |

**MWDMA**, same masks: [L][C][R][W]

| Mode | B | C |
|---|---|---|
| 0 | 0xE0 | 0x0F |
| 1 | 0x60 | 0x04 |
| 2 | 0x60 | 0x03 |

UDMA3 and 4 repeat UDMA1 and 2, and UDMA5 repeats UDMA2, because the faster
modes are a clock change (BAR4+0x11) and not a timing change. The vendor makes
this explicit: `ultra.sys` indexes one seven-record table `_MoryDMASpeed` by a
**cycle-time bucket**, not a mode number (`0x10b8e`): [W]

| Cycle time | Bucket | Bytes (B, C) | Used for |
|---|---|---|---|
| > 480 ns | none (no DMA) | | |
| > 150 ns | 0 | `E0 0F` | MWDMA0 (480 ns) |
| > 120 ns | 1 | `60 04` | MWDMA1 (150 ns) |
| > 90 ns | 2 | `60 03` | MWDMA2 (120 ns), UDMA0 (120 ns) |
| > 60 ns | 3 | `40 02` | UDMA1 (90 ns) |
| > 45 ns | 4 | `20 01` | UDMA2 (60 ns) |
| > 30 ns | 5 | `40 02` | UDMA3 (45 ns) |
| else | 6 | `20 01` | UDMA4 (30 ns), UDMA5 (20 ns) |

`fasttrak.sys` 2.00 b33 splits the same 18 bytes into `_MoryDMASpeed` (3 MWDMA
records) and `_MoryUDMASpeed` (6 UDMA records) indexed by mode number, with
identical values; when no DMA mode is selected it writes B = 0xF0, C = 0x0E
(`0x18f1c`), where `ultra.sys` skips the DMA contribution. [W]

The ROM tables (`Ul200b18.BIN`, 4 bytes per entry, bytes 0/1/2 to 0x60/0x61/0x62): [R]

| 0x1AE1 PIO | 0x1AF5 MWDMA | 0x1B01 UDMA |
|---|---|---|
| 0: 09 13 00 | 0: 00 E0 0F | 0: 00 60 03 |
| 1: 05 0C 00 | 1: 00 60 04 | 1: 00 40 02 |
| 2: 03 08 00 | 2: 00 60 03 | 2: 00 20 01 |
| 3: 02 06 00 | | 3: 00 40 02 |
| 4: 01 04 00 | | 4: 00 20 01 |
| | | 5: 00 20 01 |

v1.14 carries identical tables at 0x14AB / 0x14BF / 0x14CB. The 1999
`ULTRA66.SYS` carries the first three vendor tables at 0x14EC0 with identical
bytes. [R][W]

Raw vendor data (`ultra.sys .data 0x15AA0`; identical in `ultra.mpd` at
`0x15c40`, `ULTRA66.MPD` at `0x14ea0`, `fasttrak.sys` at `0x1e14c`): [W]

```
_MoryPIOSpeed      e9 13 00  e5 0c 00  e3 08 00  e2 06 00  e1 04 00  00
_MoryPIOSpeed_Nio  c9 13 00  c5 0c 00  c3 08 00  c2 06 00  c1 04 00  00
_MoryDMASpeed      e0 e0 0f | e0 60 04 | e0 60 03 | e0 40 02 | e0 20 01 | e0 40 02 | e0 20 01
```

(byte 0 of each DMA record is unused filler.)

The Ultra66 ROM's mode selector (`Ul200b18.BIN 0x2939`) takes UDMA from word 88
when word 53 bit 2 is set, requires the 80-conductor flag for UDMA3/4/5 (else
UDMA2 and a warning), considers UDMA5 only for revision >= 2, then MWDMA from
word 63, PIO from word 64 or word 51 capped at 4. It issues SET MULTIPLE (0xC6,
count = word 47 low) and SET FEATURES 0xEF/0x03 with `0x08|pio`, `0x20|mwdma`,
`0x40|udma`. [R]

### 7.2 TX2 parts: indexed registers 0x0C-0x13 (+0x08 for the slave)

The vendor tables are eight-byte images of indexed registers 0x0C..0x13; a PIO
record is ORed with an MWDMA or UDMA record (disjoint byte positions) and all
eight bytes are written in order. [R][W]

| Record | 0x0C | 0x0D | 0x0E | 0x0F | 0x10 | 0x11 | 0x12 | 0x13 |
|---|---|---|---|---|---|---|---|---|
| PIO0 | FB | 2B | | | | | | AC |
| PIO1 | 46 | 29 | | | | | | A4 |
| PIO2 | 23 | 26 | | | | | | 64 |
| PIO3 | 27 | 0D | | | | | | 35 |
| PIO4 | 23 | 09 | | | | | | 25 |
| MWDMA0 | | | DF | 5F | | | | |
| MWDMA1 | | | 6B | 27 | | | | |
| MWDMA2 | | | 69 | 25 | | | | |
| UDMA0 | | | | | 4A | 0F | D5 | |
| UDMA1 | | | | | 3A | 0A | D0 | |
| UDMA2 | | | | | 2A | 07 | CD | |
| UDMA3 | | | | | 1A | 05 | CD | |
| UDMA4 | | | | | 1A | 03 | CD | |
| UDMA5 | | | | | 1A | 02 | CB | |
| UDMA6 | | | | | 1A | 01 | CB | |

(blank = 0x00 in the vendor records.) Linux: PIO modes 3 and 4 turn IORDY on;
prefetch is off throughout. [L]

Locations of the same data:

| Where | PIO 0x0C/0x0D | PIO 0x13 | MWDMA | UDMA |
|---|---|---|---|---|
| `U133B15.BIN` (4-byte stride) | 0x2398 | 0x23AC (byte 3) | 0x23C0 (bytes 2-3) | 0x23CC (bytes 0-2) |
| TX2000 `bios.bin` | 0x0398 | 0x03AC | 0x03C0 | 0x03CC |
| `ultra.sys` | `_U133PIOSpeed` 0x15AD8 | | `_U133DMASpeed` 0x15B00 | `_U133UDMASpeed` 0x15B18 |
| `ultra.mpd` | `_U133PIOSpeed` 0x15c78 | | `_U133DMASpeed` 0x15ca0 | `_U133UDMASpeed` 0x15cb8 |
| `fasttrak.sys` NT4 | `_U133PIOSpeed` 0x1e18c | | `_U133DMASpeed` 0x1e1b4 | `_U133UDMASpeed` 0x1e1cc |
| Linux `pdc202xx_new.c` | `pdc_i_speed` | | `pdc_d_speed` | `pdc_u_speed` |
| CORSAC | `NewPioC`, `NewPioD` | `NewPio13` | `NewMwE`, `NewMwF` | `NewUdma10/11/12` |

All 47 non-zero bytes agree everywhere. [R][W][L][C]

Writing:

- The ROMs write one contiguous 8-register burst, 0x0C..0x13 for a master or
  0x14..0x1B for a slave, always all eight, and only when the 133 MHz flag is
  set (`U133B15.BIN 0x127B`, buffer at `cs:0x0393 + 8·drive`; `bios.bin 0x0B1A`,
  buffer at DS:0x259D). [R]
- `_SetPdc20269Speed@8` writes all eight on every call, zeros included,
  through the primary or secondary pair by channel. [W]
- CORSAC writes only the registers of the selected mode, and clears 0x10 bit 7
  before a UDMA table; it notes the part may come up with registers reading 0 or
  0xFF and applies the tables regardless. [C]

When the vendor writes the TX2 tables:

- `ultra.sys` (WinXP): **generation 5 only, and only with a UDMA6-capable
  drive** (`ext+0xB8F`, set in `_IssueIdentify@16` at `0x1074a`, which also caps
  the drive at UDMA5 otherwise); never on the PDC20268. [W]
- `fasttrak.sys` 2.00 b33: only when `_bPCI133` (0x1e64c) is set, which
  `_FindDevices@12` sets on types >= 7 with a drive reporting UDMA6
  (`0x19304`); never on type 6 (PDC20270). [W]
- See section 14 for the `ultra.mpd` reading that disagrees.

`_IssueIdentify@16` also clamps IDENTIFY word 88 by generation: `& 0x07` for
gen 1 (`0x1060d`), `& 0x1F` for gen 2 (`0x10629`); gens 3-5 capped to UDMA5 or 6
afterwards. `fasttrak.sys` ceilings: type 1 UDMA2; 2,3 UDMA4; 4,5,6 UDMA5;
7,8,9 UDMA6 (`0x18b40-0x18b76`). Both match CORSAC's `UdmaMask()`. [W][C]

### 7.3 Cable detect

| Parts | Where | Polarity |
|---|---|---|
| old | configuration word 0x50, bit 10 + channel (config 0x51 bits 2/3) | **set = 40-conductor** |
| TX2 | indexed 0x0B, bit 2, on each channel's own pair | **set = 40-conductor** |

[L][C][R][W] Every source agrees on the inverted sense. On a 40-conductor
cable every vendor component clamps UDMA >= 3 to UDMA2 and the ROMs print
"[WARNING] BECAUSE OF 40-CONDUCTOR CABLE(S) USED ... WOULD BE DOWN TO ULTRA33
MODE" (Ultra ROMs) or "WARNING: An Ultra ATA/66 (or faster) drive is connected
with a 40-pin IDE cable..." (FastTrak ROMs). An emulation that returns zeroes
reports 80-conductor cables, usually what is wanted, but it should be
deliberate. [R]

Vendor bug, no hardware bearing: `ultra.mpd` `_AtapiCheckCable@4` writes the
channel-1 TX2 clamp into channel 0's slot (`[esi+0x54]` instead of `[esi+0x56]`,
at 0x10ff2/0x10ffc). [W]

---

## 8. Interrupts and DMA

### 8.1 SFF-8038i behaviour

DMA is ordinary SFF-8038i: PRD table address as a dword to BM+4, 0x06 to BM+2
to clear the error and interrupt latches, then 0x09 (start, device to host) or
0x01 (start, host to device) to BM+0, and 0x00 to stop. The Ultra66 ROM
read-modify-writes the command and status registers (`|= 1`, `&= ~1`,
`|= 6`), so they must read back what was written. [R][W]

### 8.2 The start bit clears itself

On these parts **bus-master command bit 0 drops by itself when the PRD list is
finished**: [W][R][E]

- `ultra.sys` and `ultra.mpd` on the old parts, and `fasttrak.sys` on types < 6,
  spin up to 0x5000 reads of BM command waiting for bit 0 to fall before
  believing an interrupt, and force it down only as recovery from a stuck
  engine. [W]
- Once the ROM has set the bit it never touches it again. In the 86Box model,
  an engine that left the bit up made the ROM's interrupt handler conclude the
  interrupt belonged to someone else, chain it away, and never mark the
  transfer complete. [E]

### 8.3 How each component decides an interrupt is its own

| Component | Test | Notes |
|---|---|---|
| Linux | BAR4+0x1D bit 2 / 6 | [L] |
| Ultra66 ROM | BAR4+0x1D bits 2/6 in its IRQ handlers (`Ul200b18.BIN 0x1568, 0x15FD, 0x165D`); **if neither is set it chains to the previous vector and does no EOI**. The DMA wait (`0x05EC`) needs interrupting **and** FIFO-empty. | [R] |
| FastTrak100 ROM | BAR4+0x1D bits 2/6 (`ft100B24.bin 0x2338`), else chain | [R] |
| `ULTRA66.MPD` (1999) | polls BAR4+0x1D for FIFO empty before ATAPI DMA | [W] |
| Ultra133 TX2 ROM | installs its own handler at CS:1DD4 (second controller with a different line: CS:0x1E8B); unmasks the 8259 itself | [R] |
| TX2000 ROM | BM status bit 2 (`bios.bin 0x2522-0x253C`), reads task file +7, writes the status byte back; chains to saved vectors otherwise | [R] |
| `ultra.sys` / `ultra.mpd` 2.00.43 | **BM status bit 2 only**, both channels; never reads +0x1D. Returns FALSE if neither is set. | [W] |
| `FASTTRAK.SYS` Win2000 (FT100) | BM command bit 0, then alternate status == 0x50 or 0x51 | [W] |
| `fasttrak.sys` 2.00 b33 | BM status bit 2 only | [W] |

The consequence: **BAR4+0x1D bit 2/6 and BM status bit 2 of each channel carry
the same information, and a model must keep them in step**, rising and falling
together with the interrupt actually raised. A model that raises the PCI
interrupt without BM status bit 2 makes `AtapiInterrupt` return FALSE and
Windows eventually disables the line; one that leaves +0x1D at zero hangs every
Ultra66 BIOS transfer for the full timeout (AH = 0x80). [L][R][W]

Neither Windows driver ever tests BM status bit 1 (error). [W]

### 8.4 Acknowledgement order

`ultra.sys` / `ultra.mpd` `_AtapiInterrupt@4`, per channel: (old only) wait for
BM command bit 0 to clear, forcing it after 0x5000 reads; (old only) clear the
66 MHz bit in BAR4+0x11; read task-file status (+7); write 0x04 to BM status;
write 0x00 to BM command; (gen 3 only, when an LBA48 transfer was in flight,
flag 0x4000) write ULONG 0 to BAR4+0x20/+0x24. If both channels show bit 2,
channel 0 is serviced first. [W]

`fasttrak.sys` 2.00 b33 (`_RaidInterrupt@4` → `_GetInterruptFrom@12` →
`_IsIntHappen@12` → `_ClearInterrupt@12`): read status +7, write 0x04 to BM
status, for both channels and for a second adapter; on types 2-5 also clears
BAR4+0x11, zeroes BAR4+0x20/+0x24, and writes 0 to BM command, the last
**skipped for types >= 6**. [W]

### 8.5 nIEN and device selection quirks

`_EnableInterrupt@12` keeps a per-channel nesting count at `ext+0xA74 + 4·ch`
and writes 0x02 (nIEN set) to the device control register at the first
disable and 0x00 when the count falls to zero. Plain ATA. [W] The Ultra ROMs
release SRST with 0x00, nIEN clear; the Ultra133 TX2 ROM writes 0x0A to device
control before probing a master slot and 0x08 after the matching slave slot. [R]

The Ultra66 ROM's IRQ handler (`Ul200b18.BIN 0x15FD`) reads the device/head
register on the interrupting channel, XORs bit 0x10 (DEV), writes it back and
reads status, to force the drive to drop INTRQ. A model must tolerate a device
select flip mid-transfer. [R]

After talking to an ATAPI slave the Ultra66 ROM checks BAR4+0x1D bits 2/6 and,
if either is set, re-selects device 0 and reads status to clear the condition
(`Ul200b18.BIN 0x2572`). The Ultra133 TX2 ROM does the analogous thing keyed on
indexed 0x0B bit 5 (`U133B15.BIN 0x3158`). [R]

---

## 9. Option ROM behaviour

### 9.1 Structure

**Ultra66 `Ul200b18.BIN`** [R]

| Offset | Content |
|---|---|
| 0x00 | `55 AA`, 0x20 blocks (16 KiB), **rewritten at run time** |
| 0x03 | `jmp 0x1E15` (init) |
| 0x08 | checksum byte, recomputed at run time |
| 0x18 | PCIR pointer 0x183F |
| 0x1A | PnP header pointer 0x1877 |
| 0x1C | `"PROMISE"` |
| 0x24-0x33 | table of 8 drive-structure pointers (0x0034 ... 0x01F4), re-ordered at init so detected drives come first (0x2B3F) |
| 0x34-0x233 | 8 drive structures, 0x40 bytes each |
| 0x183E | runtime `'Y'`/`'N'` "PnP BIOS present" flag, **not** part of PCIR |
| 0x183F | PCIR: 105A:4D38, length 0x18, class `8F 80 01`, image 0x20 blocks, last image |
| 0x1877 | `$PnP`: device id `89 42 66 A1` (vendor "PTI" [?: product nibble order]), product "ULTRA   D0 " at 0x1897, class 01/80/00, indicators 0xF4, **BCV 0x16B3**, DV 0 |
| 0x16B3-0x16D6 | 8 BCV stubs, `mov bx,n; jmp 0x16D9` |
| 0x1A77 | version banner |

At init it builds a chain of `$PnP` headers, one per detected drive, at
`0x1877 + 0x40·n`, names each after the drive's IDENTIFY model string, and
recomputes each checksum (0x2ED6-0x2F65). No INT 19h hook; booting is via BCV.

**Ultra133 TX2 `U133B15.BIN`** [R]

| Range | Content |
|---|---|
| 0x00 | `55 AA`, 0x20 blocks; `jmp 0x27BE` |
| 0x18 / 0x1A | PCIR 0x20F8, `$PnP` 0x2130 |
| 0x20-0x27 | `69 4D 5A 10 68 4D 20 00` (Ultra100 TX2: `68 4D 5A 10 68 4D 20 00`); no code reads it [?: `PTIFLASH.EXE` metadata] |
| 0x2A | `"PROMISE"` |
| 0x32-0x41 | 8 pointers to 0x50-byte per-drive control blocks (0x0042 ... 0x0272) |
| 0x0000-0x0400 | ROM-resident variables, written through `cs:` after shadowing |
| 0x0400-0x20F0 | INT 13h / BCV service, ATA/ATAPI engine, El Torito, INT 13h extensions |
| 0x20F0-0x2150 | PCIR, `$PnP`, scratch (0x20F7 = `'Y'`/`'N'`) |
| 0x2150-0x2330 | "ULTRA   Dn " templates |
| 0x2330-0x2398 | version and copyright |
| 0x2398-0x23E0 | timing tables |
| 0x23E0-0x27BD | messages |
| 0x27BE-0x3EAA | POST initialisation |
| 0x3EAB-0x3FFF | zero padding |

`$PnP` at 0x2130: device ID `89 42 66 A1`, manufacturer 0x002A, class 00 01 80,
**BCV 0x1F42**.

**FastTrak100 `ft100B24.bin`**: `55 AA 80`, `jmp 0x0188`, PCIR at 0x1C
(105A:4D30, class `D8 04 01`, 0x80 blocks), `$PnP` at 0x30, product template
"FT Ary X" at 0x54. One image, no compression (per-4 KiB entropy 6.2-6.9
bits/byte). 8235 bytes (12.6 %) at 0xDFD5 are a leftover Microsoft CodeView
`NBNB09` symbol table (object list `bios\CONF.obj`, `engine\bios\STRIPE.obj`,
`RECOVERY.obj`, `AREQUEST.obj`, `bios\INT13.obj` ...; 556 publics such as
`RebuildArray`, `SecondLevelSubmitRaid`, `MakeDrvIndTab`); about 16 KiB
(0x9000-0xCFFF) is the FastBuild utility; roughly 0x4000-0x8FFF is the RAID
engine. [R]

**FastTrak TX2000 `bios.bin`**: `55 AA 80`, `jmp 0x0199`, PCIR at 0x002A
(105A:6269, class `8F 04 01`), `$PnP` at 0x0042 (class **01 80 00**, BCV
0x0326, an eight-way dispatch). Header 0x20-0x27 `69 62 5A 10 68 4D 29 00`.
No symbol table (the build was cleaned between 2.00.0.24 and 2.00.0.33);
0xE42B-0xFFFF is 7125 bytes of zero. [R]

| Range (TX2000) | Content |
|---|---|
| 0x0000-0x0060 | header, PCIR, `$PnP` |
| 0x0193-0x0325 | init entry and prologue |
| 0x0326-0x0397 | BCV entry and dispatch |
| 0x0398-0x03E7 | timing tables |
| 0x03E8-0x03FB | variables (0x3F1/0x3F2 cable word, 0x3F4 warning flags, 0x3F5 measured clock, 0x3F7/0x3F9 IRQ, 0x3FB UDMA6 flag) |
| 0x03FB-0x1800 | PCI enumeration, PLL, drive probe, mode selection, timing |
| 0x1800-0x2B00 | INT 13h, IRQ handler, DMA |
| 0x2D80-0x2DC6 | copyright string, source of the metadata magic |
| 0x2E00-0x8F00 | RAID engine |
| 0x9000-0xE42A | FastBuild |

### 9.2 POST flow and identity gates

Every ROM scans for another copy of itself and declines if one is installed:
if CS != 0xC800 it walks segments from 0xC800 up to its own CS in 2 KiB
(0x80-paragraph) steps looking for `55AA` with a PCIR of its own vendor,
device and class word, and on a match returns AX = 0x0100 (the TX2 ROMs also set
their block count to 0; the TX2000 also zeroes the PCIR image length). So a
machine with two identical cards runs only the lowest-addressed ROM
(`Ul200b18.BIN 0x304A`, `U133B15.BIN 0x3CC1`, `ft100B24.bin 0x0DF5`,
`bios.bin 0x0E96`). The FastTrak message is "TWO CONTROLLER WARNING: System
BIOS does not support multiple Promise adapters." [R]

All PCI access is through INT 1Ah PCI BIOS (B102, B108/B109, B10B/B10C, B10E or
B10F); none of the ROMs uses 0xCF8/0xCFC. None touches an EEPROM, serial NVRAM
or flash part; configuration lives in the ROM image, rewritten by
`PTIFLASH.EXE`. [R]

| Gate | Ultra66 | Ultra133 TX2 | FastTrak100 | FastTrak TX2000 |
|---|---|---|---|---|
| device searched | 4D38 | 4D69 (v12 and Maxtor also 4D68) | 4D30 | 6269 only |
| class word 0x0A | 0x0180 | 0x0180 | 0x0104 | 0x0104 |
| subsystem 0x2E | **0x4D33** | not read | **0x4D32** | not read |
| BAR0/BAR2 bit 0 required | yes | yes | no | no |
| revision read | yes (UDMA5 gate) | no | no | no |
| config 0x40 | | written 0 | | written 0 |
| config 0x50 | read once (cable) | not read | read (cable, bit 6) | not read |
| adapters | only the lowest ROM; `[cs:0x1558]` controller count | at most 2 | up to 4 | up to 4 |
| BAR5 | | never touched | | never touched |

Failure messages: "Can get base I/O port addresses, please use compatible mode"
(BAR0/BAR2 without bit 0), "Ultra66 - Can not get bus master base port
address" / "Ultra133TX2 - Can not get bus master base port address" (BAR4),
"Can not find the PCI Device - PDC20262" / "- PDC20269", "Ultra133TX2 BIOS is
not installed because there are no drives attached.", "No FastTrak Controller
Found". [R]

**Ultra66 init sequence** (0x1E15): save ES:DI (`$PnP` install check); duplicate
check; banner; steal 0x3B8 bytes below 40:13 for DS (0x2009); validate `$PnP`;
PCI enumeration (0x2068); build 8 drive structures (0x220E); reset channels and
detect (0x2318); hook the IRQ vector(s) (0x2413); unmask in the 8259 (0x24D8);
per-drive IDENTIFY, geometry, mode set; compact the drive table and HDPTs
(0x2B3F); hook INT 13h/15h/41h/46h (0x2CC7); 40-conductor warning (0x2E8E);
PnP chain (0x2ED6); shadow the runtime (0x2FAC); shrink the ROM (0x2F66); return
AX = 0x110. POST codes to port 0x80 (routine 0x309A): 0xF1 memory allocated,
0xF2 PCI enumerated, 0xF3 structures built, 0xF4 detection done, 0xF5 IRQ
hooked, 0xF6 PIC unmasked, 0xFA per drive reported. [R]

**Ultra133 TX2 init sequence** (0x27BE): duplicate check (0x3CC1); print
(0x30F6); steal 0x59E bytes (2 KiB) below 0040:0013 for DS (0x2A88), **without
writing the reduced size back**; validate `$PnP` (0x2AB4); PCI enumeration
(0x2AE7); eight control blocks (0x2D26); soft reset, PLL measurement, drive
detection (0x2E9B); hook the IRQ vector (0x302A); unmask (0x30B3); PLL
programming (0x2809); per-drive cable, IDENTIFY, mode, SET FEATURES, timing
(0x2891); INT 13h/BCV installation (0x36BA/0x3765/0x37B4/0x38A2); return AX =
0x0110 or 0x0100 (0x2A62). Its only fixed-port accesses are the 8259s. [R]

**TX2000 init** (0x0199): duplicate scan (0x0E96); save ES:DI; `call 0x94E0`;
steal base memory (0x2B10); banner; "Scanning IDE drives "; PCI enumeration
(0x47B0 → 0x03FB), which includes cable detect and the PLL measurement; build
the drive table; hook INT 13h and the interrupt; run the array code and
FastBuild. [R]

### 9.3 Drive detection

Soft reset on the device control port only: 0x04 (SRST), about 200 iterations
of delay, 0x00. No chip register is touched during reset. On the Ultra133 TX2
the ~2 s PLL measurement that follows doubles as settling time; the TX2000
measures before touching any drive. [R]

Presence (`Ul200b18.BIN 0x2351`, `U133B15.BIN 0x2EDD`): write 0xA0/0xB0 to +6,
read status: 0xFF absent; BSY retry until timeout; ERR absent;
`(status & 0xD0) == 0x50` ATA (flag 0xFF); `+5 == 0xEB && +4 == 0x14` ATAPI
(flag 0xEE). The Ultra133 TX2 timeout is 180 ticks (≈9.9 s) for slot 0 and 18
ticks (≈1 s) for every other slot. The FastTrak ROMs and Windows drivers
instead scratch-test: select 0xA0/0xB0, write 0xAA to +4 (cylinder low), read it
back. [R][W]

ATA: IDENTIFY (0xEC), `rep insw` 256 words. ATAPI: DEVICE RESET (0x08), then
IDENTIFY PACKET DEVICE (0xA1), three retries; an all-blank model becomes
"ATAPI DEVICE". [R]

IDENTIFY words consumed by the Ultra133 TX2 ROM: 0 bit 7 (removable, then a
media-status probe), 47 low (multiple count → SET MULTIPLE), 49 bit 8 (DMA),
49 bit 9 (LBA), 51 high (PIO fallback, <= 4), 53 bits 1/2 (words 64-70 / 88
valid), 63 bits 2:0 (MWDMA), 64 bits 1:0 (PIO 4/3), 82 bits 10 and 0, 83 bits
14 and 10 (48-bit LBA), 88 bit 0 (required for any UDMA), 88 bits 6:2 (highest
UDMA), **88 bit 6 (133 MHz flag)**. `[cs:0x3D6]` bits 0x10/0x20, never set in
the image, disable DMA for controller 0 / 1 [?: patched by a configuration
utility]. [R]

The TX2000 mode selector uses cycle-time ladders: word 64 against
`0x1E0, 0x168, 0xD2, 0xB4, 0x96, 0x78, 0x5A, 0x3C` for PIO 0-7, and word 67/68
against `0x186, 0x12C, 0xF0, 0xB4, 0x78, 0x5A, 0x3C` for MWDMA index 1-9; the
FastTrak100 ROM uses the same MWDMA thresholds. [R]

The Ultra133 TX2 ROM boots CD-ROMs through El Torito (0x3D64): sector 0x11, then
the boot catalog, tests `[0] == 0x01`, `[1] == 0x00`, `[0x1E] == 0xAA55`,
`[0x20] == 0x88`; up to three retries with a 28-tick delay and a controller
reset between them; prints "A BOOTABLE CD WAS DETECTED IN THE CD-ROM DRIVE." [R]

### 9.4 INT 13h hooking and the INT 40h relocation

`Ul200b18.BIN 0x2CC7`: [R]

```
00002CCE  mov al,[es:0x75]     ; 40:75 fixed-disk count
00002CE0  mov [es:0x75],al     ; += number of drives found
00002CF8  mov ax,[es:0x4c]     ; if the OLD count was 0, copy INT 13h
00002CFC  mov [es:0x100],ax    ;   to INT 40h (floppy revector)
00002D29  mov ax,0x1527        ; INT 15h -> cs:0x1527 (AH=0x52 only)
00002D36  mov ax,0x959         ; INT 13h -> cs:0x959
00002D4F  mov ax,0x1857        ; INT 41h (0:104) -> HDPT, drive 0
00002D66  mov ax,0x1867        ; INT 46h (0:118) -> HDPT, drive 1
```

The INT 13h hook passes `dl < [cs:0x2DE]` (first owned drive) or
`> [cs:0x2DF]` (last) to the old vector. The 16-byte fixed-disk parameter
tables at 0x1857/0x1867 are filled from the drive structure. [R]

The same relocation is present in the TX2 ROMs, checked with ndisasm for this
document: `U133B15.BIN 0x38B9-0x38EF` (saves the old 40:75 at `cs:0x20F1`, adds
its drive count, and copies INT 13h to INT 40h when the old count was 0) and
again at `0x1FB3-0x1FCC`; `bios.bin 0x1810-0x182C` copies INT 13h to INT 40h
when `[0x20F4]` is 0 before installing its own INT 13h at CS:0x1877, and
`0x1842-0x185F` restores INT 13h from INT 40h when 40:75 is 0. [R]

The logic assumes that when 40:75 is zero, the INT 13h vector is the system
BIOS's floppy-only handler, which is the one INT 40h is meant to hold.
[?: If another disk option ROM has already hooked INT 13h without adding drives
to 40:75, the Promise ROM copies *that* ROM's entry point into INT 40h. When the
other ROM then passes a floppy request on to INT 40h, as floppy-relocating
BIOSes do, INT 40h points back into itself and the request loops. This was
observed as an interaction, not traced through a specific second ROM.]

### 9.5 IRQ hooking

- **Ultra66**: hooks the controller's IRQ vector(s), handler port list at
  `[cs:0x155A..0x1564]`, old vectors at `[cs:0x1830..0x1836]`; unmasks in the
  8259. Calls B10E (GET_IRQ_ROUTING_OPTIONS, 0x1FA-byte buffer at DS:0x50) only
  to cache the slot number byte (+0x0E), which nothing later uses [?: display
  only]. [R]
- **Ultra133 TX2** (`0x302A`): maps the config 0x3C line to an IVT slot
  (IRQ0-7 → INT 08h-0Fh, IRQ8-15 → INT 70h-77h), installs CS:1DD4, saves the old
  vector at `cs:0x20E9`; a second controller on a different line gets
  CS:0x1E8B and `cs:0x20ED`; `0x30B3` clears the mask bit in port 0x21 or
  0xA1. [R]
- **FastTrak100 / TX2000**: call PCI BIOS **B10F SET_PCI_IRQ** (TX2000:
  `cx = (line<<8) | 0x0A`, `ds = F000h`), i.e. may reprogram the PCI interrupt
  router, and write the line back to config 0x3C. If B10F fails they set bit 1
  of a flag byte (`ft100B24.bin cs:0x0440`, `bios.bin cs:0x3F4`) and keep the
  line read from 0x3C. The FastTrak100 then prints "Warning - FastTrak does not
  detect proper interrupts, / Please check your PCI IRQ setup, and make sure
  the PCI slot / support Bus Master operations" (0x2227); the TX2000 prints the
  two-controller warning text for the same flag (0xA407) [?: shared flag bit or
  string mix-up]. The TX2000 does not unmask the 8259 itself. [R]

### 9.6 Memory: base memory, EBDA, shadowing

- **Ultra66**: takes 1 KiB from 40:13 for a 0x3B8-byte scratch DS; later
  (0x2FAC) reserves `ceil(0x92E/1024)` KiB more (fudged so the result is not
  ≡ 0 mod 4 and not 0x27C/0x27B), copies ROM bytes 0x0000-0x092D there, stores
  the segment at `[cs:0x2E9]`, patches `[cs:0x238]` with the 32-bit physical
  address of offset 0x25C in the copy (the PRD table), and far-jumps INT 13h
  AH=02/03/42/43 into it (0x9E5), so the data path runs from RAM. It then
  (0x2F66) rewrites the block count at 0x02 and the PCIR image length (0x184F) to
  `0x1878 + 0x40·ndrives` rounded up to 2 KiB and recomputes the checksum,
  shrinking to as little as 8 KiB so the system BIOS reclaims the rest. v2.00
  also adds VDS (INT 4Bh) use in the PRD builder, gated on 40:7B bit 0x20. [R]
- **Ultra133 TX2**: 2 KiB for DS; does **not** write 40:13 back. [R]
- **FastTrak100**: takes 24 KiB below the top of base memory for a private
  stack (SP = 0x8000), allocates 10 KiB zeroed for the RAID engine's DS, and on
  success permanently reduces 40:13 (gives the 10 KiB back on failure,
  `add byte [es:0x13],10` at 0x025E). [R]
- **TX2000** (`0x2B10`): refuses if 40:13 is already 0x280 or 40:0E is 0;
  otherwise relocates the existing **EBDA** at 40:0E upward by 10 KiB, adds 10 to
  the EBDA size byte, and adjusts 40:13. [R]

### 9.7 What the ROMs need from the chip

**Ultra66** (values branched on): class 0x0180; subsystem 0x4D33; revision >= 2
for UDMA5; BAR0/BAR2 bit 0; BAR4 read OK; config 0x50 bits 10/11; **BAR4+0x1D
bits 0/2 (ch0) and 4/6 (ch1) live**; BM command/status read back for RMW;
status 0x50 for a present ATA drive; ATAPI signature 0x14/0xEB; status bits 0/3
for completion; control-port bits 0x89 during ATAPI reset. Writable, never read:
config 0x60-0x6F, BAR4+0x11, BAR4+0x1A/0x1B, BAR4+0x1F. [R]

**Ultra133 TX2 / TX2000**: class word; BAR bits; the PLL counter with the
semantics of section 6; indexed 0x0B bit 2; indexed 0x01 read-back for RMW; the
usual task-file status values; for the TX2000, BM status bit 0 clear before it
will start a transfer (`bios.bin 0x2D33`). [R]

---

## 10. Windows driver behaviour

### 10.1 `ultra.sys` 2.00.43 (NT family)

`_DriverEntry@8` (0x154a8) installs: [W]

| Entry | Routine |
|---|---|
| HwInitialize | `_AtapiHwInitialize@4` 0x11994 (= `AtapiScanDevice`, `AdjustAllParameter`, `AtapiInitDevice`) |
| HwResetBus | `_AtapiResetController@8` 0x10866 |
| HwStartIo | `_AtapiStartIo` 0x15422 |
| HwInterrupt | `_AtapiInterrupt@4` 0x127ca |
| HwFindAdapter | `_AtapiFindPCIController@24` 0x12290 |
| HwAdapterControl | `_AtapiHwScsiAdapterControl` 0x15610 |
| HwAdapterState | `_AtapiAdapterState` 0x10860 |

DeviceExtensionSize 0xB94, SpecificLuExtensionSize 4, SrbExtensionSize 0x120,
**NumberOfAccessRanges 6** (BAR0-BAR5), AdapterInterfaceType 5 (PCIBus),
MapBuffers / NeedPhysicalAddresses / TaggedQueuing = 1. Hardware only through
`ScsiPortRead/WritePort*`, `ScsiPortRead/WriteRegister*` (memory space),
`ScsiPortGetBusData`, `ScsiPortSetBusDataByOffset`, `ScsiPortGetDeviceBase`;
no `in`/`out` in the image. [W]

**Identification.** `_FindBrokenController@20` (0x11ce0, NT4-style scan of bus 0
slots 0-0x1F) and `_CheckControllerTypeStrict@8` (0x11ea8, called from
`_FindPromiseControllerData@12`) read the whole 256-byte header and check, per
device ID, vendor 0x105A, class 0x01 and subclass (0x04 for 4D33, 0x80
otherwise); a mismatch means the driver does not load (`0x11f23`, `0x11f29`).
`ULTRA.INF` claims `4D69`, `4D68`, `4D30`+`SUBSYS_4D33105A`,
`0D30`+`SUBSYS_4D33105A`, `4D38`+`SUBSYS_4D33105A`, `4D33`. Not claimed: 0D38,
6268, 6269, 5275, 7275. [W]

**Probe** (`_AtapiFindPCIController@24`): identify; latch BARs and config 0x50
(cached at `ext+0xA94`); clear config 0x42 bit 0; on gens >= 4 read the PLL pair
(section 6.6); on gen 5 map BAR5; software-reset both channels (0x04 to device
control, 50 × 1000 µs, 0x00, then poll alternate status until it reads **exactly
0x50 or 0x00**, up to 1 000 000 × 5 µs). [W]

**Init** (`_AtapiInitDevice@4`): per drive SET FEATURES and timing
(`_FindPromiseSpeed@8` for gens 1-3, `_SetPdc20269Speed@8` for gen 5 with
UDMA6); gens 1-3: BAR4+0x1F ← 0x01, +0x1A ← 0x01, +0x1B ← 0x01; gen 3 only:
+0x1C |= 0x03, +0x1F &= 0x3F then |= 0x80 iff config 0x50 bit 6; gens >= 4:
clear indexed 0x10/0x18 bit 7 for all four drives, then restore the PLL pair.
[W]

**Cable check** `_AtapiCheckCable@4` (0x10e1e): config 0x51 bits 2/3 on gens
1-3, indexed 0x0B bit 2 on gens 4-5; clamp to UDMA2. [W]

**Reset** `_AtapiResetController@8`: gens >= 4 indexed 0x01 `&= 0xF3`, 1000 µs,
`|= 0x0C`; gens 1-3 BAR4+0x11 ← 0, BM command ← 0 on both channels, BAR4+0x1F
bit 4 pulsed for 50 ms; then complete every SRB with `SRB_STATUS_BUS_RESET`
(0x0E) and re-run `AtapiInitDevice`, which reprograms timing and restores the
PLL. [W]

**Timeouts** (`_AtapiTimer@4` 0x14410 and elsewhere): [W]

| Site | Condition | Bound | On expiry |
|---|---|---|---|
| 0x12665 | alt status not 0x50/0x00 after SRST | 1 000 000 × 5 µs | continue |
| 0x117b9 | alt status BSY | 0x4E20 × 150 µs | continue |
| 0x12822 | BM command bit 0 stuck | 0x5000 reads | force `&= 0xFE` |
| 0x14392 | BM bit 0 stuck before ATAPI DMA | 0x1388 reads | return FALSE |
| 0x13df2 | BM bit 0 stuck before disk DMA | 0x10000 reads | start anyway |
| 0x135c7 | BAR5 alt status BSY | 0x1388 × 5 µs | fail SRB (5) |
| 0x138e8 | BAR5 status BSY | 0x1388 × 5 µs | fail SRB (5) |
| 0x13920 | BAR5 status DRQ never sets | 0x3E8 × 200 µs | complete, zero residual |
| 0x10832 | DRQ still set after IDENTIFY | 0x10000 words | stop draining |
| 0x14443 / 0x14476 | indexed 0x0B `& 0xA0 == 0x20` | | force-stop the bus master |

The BAR5 path (`_IdeReadWrite133@8`) fails an SRB immediately (code 4) if memory
alternate status lacks DRDY (0x40), and with code 5 if BSY stays set. Every poll
escapes on 0xFF, so **an absent device must read 0xFF**. [W]

Vendor routine names from the symbol table: `_DriverEntry@8`,
`_AtapiFindPCIController@24`, `_FindBrokenController@20`,
`_CheckControllerTypeStrict@8`, `_FindPromiseControllerData@12`,
`_AtapiHwInitialize@4`, `_AtapiScanDevice@4`, `_AtapiInitDevice@4`,
`_IssueIdentify@16`, `_AtapiCheckCable@4`, `_FindPromiseSpeed@8`,
`_SetPdc20269Speed@8`, `_AtapiInterrupt@4`, `_AtapiResetController@8`,
`_AtapiTimer@4`, `_EnableInterrupt@12`, `_IdeSendCommand@8`, `_IdeReadWrite@8`,
`_IdeReadWrite133@8`, `_PreparePRDTable@8`, `_PreparePRDTable133@8`,
`_SubmitPromiseAtapiDMACommand@8`, `_StopPromiseAtapiDMACommand@8`,
`_MapError@8`; data `_MoryPIOSpeed`, `_MoryPIOSpeed_Nio`, `_MoryDMASpeed`,
`_U133PIOSpeed`, `_U133DMASpeed`, `_U133UDMASpeed`. [W]

### 10.2 `ultra.mpd` 2.00.43 and `ULTRA66.MPD` 1999 (Windows 9x)

`ultra.mpd` is the same source as `ultra.sys`: same five-way type code at
`ext+0xB74`, same class gating (`0x1201d`, `0x12023`, `0x12073`; rejection at
`0x120b7`), same BAR5 window (mapped also through VMM `_MapPhysToLinear`), same
tables, same config 0x42 clear. Named entry points: `_FindPromiseSpeed@8`
0x10c52, `_SetPdc20269Speed@8` 0x10e78, `_AtapiCheckCable@4` 0x10f56,
`_AtapiInitDevice@4` 0x11166, `_FindDevices@12` 0x11afc,
`_FindBrokenController@20` 0x11e2a, `_CheckControllerTypeStrict@8` 0x11ff2,
`_FindPromiseControllerData@12` 0x120be, `_AtapiFindPCIController@24` 0x123da,
`_AtapiInterrupt@4` 0x1286e, `_PreparePRDTable@8` 0x1349f,
`_PreparePRDTable133@8` 0x13552, `_IdeReadWrite133@8` 0x1360e,
`_IdeReadWrite@8` 0x13a36, `_SubmitPromiseAtapiDMACommand@8` 0x14362,
`_StopPromiseAtapiDMACommand@8` 0x14490, `_EnableInterrupt@12` 0x15a22. [W]

`ULTRA66.MPD` (1999) is a one-chip driver with no memory-space accessors and no
chip-type byte (globals `_lBusMasterBase` 0x14f8c, `_lSlotNum`, `_lBusNum`,
`_gAdapterCount`). `_FindBrokenController@36` compares registry strings "4D38"
and "105A"; `_FindPromiseControllerData@12` (0x12164) requires class 01:80 and
**subsystem device 0x4D33** (`0x1221b`). It writes BAR4+0x1F, +0x1A, +0x1B to
0x01 in `_AtapiHwInitialize` (0x11a45), polls BAR4+0x1D for FIFO empty before
ATAPI DMA, writes BAR4+0x20 for ATAPI DMA, and carries a request-sorting
elevator (`_SortQueue@16`, `_GetSRBFromQueue@12`, `_blSRBQueueFull@8`; registry
`Sorting`, `QDepth`, `PhysicalBreak`, `TransferMode`) that 2003 removed. [W]

1999 → 2003, in hardware terms: five-way silicon dispatch; the TX2 indexed
register file; the `_U133*Speed` tables (the `_Mory*` tables survive
unchanged); the BAR5 window; cable detect in indexed 0x0B for TX2; config 0x42
bit 0 cleared; the gen-3 BAR4+0x1C/+0x1F fix-ups; the BAR4+0x1D FIFO poll
before ATAPI DMA dropped. [W]

Drive quirk lists in `ultra.mpd` (no hardware bearing): `_Atapi_Black_List`,
`_Ide_Black_List`, `_QCommand_Device_List` (byte-swapped "IBM-", "IBM-DTTA",
"CD-524E", "Pioneer DVD-ROM ATAPIDMODEL DVA-2001X08", "CD-ROM 42X MAX"); a
"CD-ROM  CDR" model match forces mode 3. [W]

### 10.3 `FASTTRAK.SYS` (FastTrak100 era, Win2000)

`DriverEntry` (0x1A534) probes 105A:0D30, 4D30, 4D38, 0D38, 4D33 (types 5, 4,
2, 3, 1 at `ds:0x1CFF8`). `HW_INITIALIZATION_DATA` at 0x1A5E6: HwInitialize
0x18B0C, HwStartIo 0x1A456, HwInterrupt 0x1981A, HwFindAdapter 0x1943C,
HwResetBus 0x17FF0, HwAdapterState 0x18EBA, DeviceExtensionSize 0x55C,
SrbExtensionSize 0x100, NumberOfAccessRanges 0x10. Requires class 0x01 /
subclass 0x04 (`0x19071`, `0x1907A`) except for 4D33, which is accepted without
the class test. Its **only config write is command = 0x0007**; it **never
programs the 0x60-0x7F timing block**, inheriting the option ROM's values.
HwFindAdapter registers four 8-byte I/O ranges; HwInterrupt walks an 8-entry
adapter table, tests BM command bit 0 and accepts alternate status 0x50 or
0x51. [W]

The NT4 FastTrak100 TX4 build (PDC20270) retains 541 symbols, among them
`_GetInterruptFrom@12` 0x1a152, `_ClearInterrupt@12` 0x1a2e6,
`_RaidInterrupt@4` 0x1a37e, `_HardResetChannel@4` 0x1856c,
`_RaidResetController@8` 0x1869e, `_RaidFindPCIController@24` 0x19b80,
`_wReservedSectorLBA@12` 0x1b7c6, `_blReadReservedSector@16` 0x11e58,
`_blWriteReservedSector@16` 0x11ef0, `_ReadWReg@16`, `_ReadVoltage5V@8`,
`_ReadVoltage12V@8`, `_LEDControl@12`, `_bCheckOEMBoxSwap@8`,
`_bOEMSwapBoxPowerOnOff@12`, `_bOEMSwapBoxGetPowerStatus@12`; data
`_MoryPIOSpeed` 0x2ac24, `_MoryDMASpeed` 0x2ac5c, `_MoryUDMASpeed` 0x2ac74. It
probes 105A:6268 first (chip type 6), then the five old IDs; its INF adds
`PCI\VEN_105A&DEV_6268&SUBSYS_4d68105A` "FastTrak100 TX/LP". [W]

### 10.4 `fasttrak.sys` 2.00 build 33 (FastTrak TX2000 package)

`_DriverEntry@8` (NT4 0x1b5b8) counts adapters per candidate ID with
`_GetNumberOFFasttrak@4` (0x1c0d2), in the order 6268, 6269, 5275, 7275, 0D30,
4D30, 4D38, 0D38, 4D33, keeping the type of the ID that produced adapters.
`_GetNumberOFFasttrak@4` builds CONFIG_ADDRESS by hand and reads register 0
through **raw 0xCF8/0xCFC**, function 0 only, with no class check; on a machine
where 0xCF8 is not the configuration mechanism it counts zero adapters and gives
up. `_GetFastTrakDeviceID@4` (0x1d5ba) is the inverse map. [W]

`HW_INITIALIZATION_DATA` (0x1b718, NT4 0x4C form): HwInitialize
`_RaidHwInitialize@4` 0x1913c, HwStartIo `_RaidStartIo@8` 0x1b28a,
HwInterrupt `_RaidInterrupt@4` 0x1a30c, HwFindAdapter
`_RaidFindPCIController@24` 0x19a1c, HwResetBus `_RaidResetController@8`
0x18566, HwAdapterState `_RaidAdapterState@12` 0x1955a; DeviceExtensionSize
0x55C, SrbExtensionSize 0x100, **NumberOfAccessRanges 5**, TaggedQueuing 0.
**`_RaidResetController@8` is a stub (`mov al,1; ret 8`)**: bus recovery is
private (`_ChannelTimedout@4`, `_HardResetChannel@4`, `_ResetController@4`). [W]

Identification in `_RaidFindPCIController@24`: with PnP (2000/XP/2003),
`_PnPFindPromiseController@16` (0x19560) checks only the device ID; in NT4
detected mode, `_FindPromiseController@16` (0x19708) walks bus 0x00-0x0F and
requires vendor 0x105A, **class 0x01, subclass 0x04 for every part, the
PDC20246 included**, then the device ID. `FASTTRAK.INF` claims `4d33`,
`4D38&SUBSYS_4D39105A`, `4D30&SUBSYS_4D39105A`, `4D30&SUBSYS_4D32105A`, `6268`,
`6269` ("Promise FastTrak TX2000 (tm) Controller"); not 5275/7275, which are
reachable only through the NT4 scan. The 2000/XP/2003 INFs also install a null
service for `SCSI\ProcessorPromise_RAID_CONSOLE__`. [W]

Configuration writes: exactly two, the old-family timing dword at
`0x60 + 4·n` and, on types >= 6, **word 0x40 ← 0x0000** (`0x19091`). It never
touches 0x42, never reads 0x50 bit 6, never writes the command register. [W]

I/O: SFF bus master (`0x09`/`0x01`/`0x00`; bit 0 polled up to 0x5000 times
then forced down); on TX2, indexed 0x01 (reset), 0x02/0x03 (PLL, preserve),
0x0B (cable, probe only), 0x0C-0x1B (timing), 0x10/0x18 bit 7; on old parts,
BAR4+0x11, BAR4+0x1F, BAR4+0x20/+0x24. **Never** BAR4+0x1A, +0x1B, +0x1C, +0x1D,
+0x1E, and never BAR5. [W]

Resets: `_ResetController@4` (section 5.2 / 5.4); `_HardResetChannel@4`
(0x1842e): SRST 150 ms, release 150 ms, select 0xA0/0xB0 150 ms, poll alternate
status for BSY clear up to 0x7A120 × 10 µs (5 s), accept
**`(status & 0xFC) == 0x50`**, status 0x00 → up to 0x14 retries, otherwise up
to 0x64 retries 100 ms apart re-asserting SRST. Per-drive re-init after every
reset: IDENTIFY, SET FEATURES 0xEF/0x03, SET FEATURES 0x02/0x82 (write cache per
the registry value `writebackcache`), timing, the 0x10/0x18 clear. [W]

### 10.5 What each driver checks strictly

| Check | `ultra.sys`/`ultra.mpd` 2003 | `ULTRA66.MPD` 1999 | FT100 `FASTTRAK.SYS` | `fasttrak.sys` b33 |
|---|---|---|---|---|
| vendor 0x105A | yes | yes | yes | NT4 scan path |
| class 01 | yes | yes | yes (not 4D33) | NT4 scan path |
| subclass | 04 for 4D33, 80 others | 80 | 04 (not 4D33) | 04 for all (scan path) |
| subsystem | via INF only | **0x4D33** in code | via INF | via INF |
| alt status after SRST | exactly 0x50 or 0x00 | | 0x50 or 0x51 (interrupt path) | `& 0xFC == 0x50` |
| interrupt ownership | BM status bit 2 | BAR4+0x1D (ATAPI) | BM command bit 0 + alt status | BM status bit 2 |
| PLL | preserve, restore after every reset | n/a | n/a | preserve, restore on resume only |
| BAR5 | gen 5 | no | no | never |

[W]

---

## 11. FastTrak (RAID) specifics

### 11.1 Same silicon, different identity

The FastTrak100 and the Ultra100 are both the PDC20267; the FastTrak TX2000 and
the Ultra133 TX2 are the same register-level part (PDC20271 vs PDC20269). The
RAID is software. [R][W]

| | Ultra100 (PDC20267) | FastTrak100 (PDC20267) | Ultra133 TX2 (PDC20269) | FastTrak TX2000 (PDC20271) |
|---|---|---|---|---|
| device ID | 4D30 | 4D30 | 4D69 | **6269** |
| class:subclass | 01:80 | **01:04** | 01:80 | **01:04** |
| subsystem demanded by ROM | 4D33 | **4D32** | none | **none** |
| option ROM | 16 KiB | 64 KiB | 16 KiB | 64 KiB |
| adapters per ROM | | 4 | 2 | 4 |
| IRQ setup | hook vector, unmask | B10F SET_PCI_IRQ | hook vector, unmask | B10F SET_PCI_IRQ |
| base memory | 1-2 KiB | 10 KiB (+ stack area) | 2 KiB, 40:13 not updated | 10 KiB, EBDA moved |
| BAR5 memory window | none | none | used by `ultra.sys` | **not used by any FastTrak code** |

The TX2000 ROM uses the same index/data pair, the same indexed register numbers,
the same PLL arithmetic down to the constants (36 ticks, `~C & 0x3FFFFFFF`,
`/200000`, 15000/13300, R 0x0D/0x08, the same divide-by-zero hazard), the same
cable bit and polarity, and byte-identical timing tables as the Ultra133 TX2
ROM. It searches for 6269 alone (the bytes `69 4D` occur nowhere in its 64
KiB). [R] Its old-family timing routine at 0x09E7 is dead code (`call 0xe54;
jmp 0xa98` skips 0x09EE-0x0A97), so it never uses the 0x60-0x7F block, config
0x50, BAR4+0x1A-0x1F or BAR4+0x20/+0x24 on a live path. [R] One legacy read
survives: config 0x62/0x6A bit 6 (section 4.3); a model should return zero
there.

`$PnP` class in the TX2000 ROM still says 01:80; only PCIR and real config
space say RAID. [R]

### 11.2 Registers the FastTrak100 ROM uses that the Ultra66 ROM does not

A histogram of `add dx,byte +N` over each disassembly: [R]

```
ft100B24.bin : 0x01 0x02 0x03 0x04 0x06 0x07 0x08 0x11 0x1A 0x1B 0x1C 0x1D 0x1F 0x20
Ul200b18.BIN : 0x01 0x02 0x03 0x04 0x06 0x07
bios.bin     : 0x01 0x02 0x03 0x04 0x06 0x07 0x08 0x09 0x0B     (TX2000)
```

The FastTrak100 ROM writes BAR4+0x1E ← 0x00 (0x04B0, first thing after reading
BAR4), +0x1C low nibble ← 3, +0x1F |= 0x01 (|= 0x80 iff config 0x50 bit 6),
+0x1A ← 1, +0x1B ← 1 (once per chip), reads +0x1D in its IRQ handler, and writes
+0x20/+0x24 for ATAPI/DMA. The plain ROM is an INT 13h polling BIOS with no
interrupt handler of that kind, which is why it has no use for +0x1D and
+0x20; both are documented for the plain part in Linux and used by the plain
Windows driver, so they are the same silicon. Nothing the Ultra66 ROM touches is
absent from the FastTrak100 ROM. [R][L][W]

### 11.3 The PDC20270 code paths

The FastTrak100 ROM contains the TX2 indexed code compiled out by a
constant-false guard (`ft100B24.bin 0x0E69`: `mov ax,0x6268; cmp ax,0x4D30;
jnz`), which would clear bit 7 of indexed 0x10 through BAR4+8·ch+1/+3. [R]

The TX2000 ROM has the mirror image: its tHOLD clear (indexed 0x10/0x18
`and 0x7F` on the secondary pair of up to four adapters, body 0x0F1A-0x0F44,
helper 0x0F47) is guarded by `mov ax,0x6268; cmp ax,0x6269; jnz` at 0x0F11 and
never runs. [R] [?: a `PRODUCT_ID` macro set to 0x6268 in this build, with the
identity constants set to 0x6269.]

The PDC20270 (FastTrak100 TX2/TX4/LP) has four channels as two PCI functions
behind a bridge (Linux `pdc20270_get_dev2()`); the TX4 driver's
`_GetInterruptFrom@12` loops channels 0-7, switching to a second per-adapter
block at devext +0x31C for channels >= 4, and uses NumberOfAccessRanges 5. [L][W]

The TX4 driver's `_ReadVoltage5V@8`, `_ReadVoltage12V@8`, `_LEDControl@12`,
`_bOEMSwapBox*` and the TX2000 driver's `_Swap_Enter_State@8`, `_Set_SDA0/1@4`,
`_Get_SDA@4`, `_Sel_BUS0@4`, `_WriteByte@8`, `_ReadByte@4`, `_ReadWinBondId@8`,
`_ReadTemperature@8`, `_ReadFanStatus@8`, `_SetupFanDivisor@8` drive an
external SuperSwap hot-swap enclosure, not the chip: an I2C bus to a Winbond
hardware monitor bit-banged on **task-file +3** (shadow byte `_ax` 0x1e664,
bit 0x08 = SDA, bit 0x02 = SCL strobe), after selecting 0xA0/0xB0 at +6. With
no cage the reads return 0xFF/0x00 and no enclosure is reported. [W]

### 11.4 Subsystem IDs

The FastTrak100 ROM demands subsystem device **0x4D32** (`ft100B24.bin 0x048E`),
but every FastTrak INF binds the FastTrak66/100 to **`SUBSYS_4D39105A`**
(`FASTTRAK.INF` in the TX2000 package also lists `4D30&SUBSYS_4D32105A`). The
TX2000 ROM checks no subsystem ID, and `FASTTRAK.INF` claims 6268 and 6269 with
no subsystem constraint. [R][W]

### 11.5 On-disk metadata (the "reserved sector")

Named `_wReservedSectorLBA@12`, `_blReadReservedSector@16`,
`_blWriteReservedSector@16`, `_InitRsvdSector@12`, `_InitNewRsvdSector@12`,
`_blAbortWriteToReservedSector@4`, `_blPreReadProtectedSector@4`,
`_wGetChecksum@8`. A 2048-byte block (four sectors): [R][W]

| Offset | Size | Content |
|---|---|---|
| 0x000 | 24 | magic `"Promise Technology, Inc."` = characters 20-43 of `"Copyright (c) 1995, Promise Technology, Inc.  All rights reserved."` (FT100 driver `.rdata:0x1C838`; TX2000 ROM `cs:0x2D94`) |
| 0x018 | 4 | 0x00020000 (TX2000 validator requires word `[0x1A] == 2`) |
| 0x01C | 8 | per-disk identity (from channel, target) |
| 0x024 | ... | further identity |
| 0x200 | 1 | 0x80 |
| 0x204 | ... | descriptor array; first record `[0]=3` type, `[2]` channel, `[3]` target, `[0x10]` reserved-sector LBA, `[0x14]` 0xFFFFFFFF |
| 0x21C | 0x78 | descriptor table, zeroed on create |
| record +0x20 | 4 each | per-member LBAs, `(member − this_member)·4 + 0x20` |
| 0x7FC | 4 | 32-bit checksum of bytes 0x000-0x7FB |

Format stamp: `"Promise Not Yet Defined 1.0397121712"` in both ROMs
(`ft100B24.bin 0x5B78`, `bios.bin 0x5D8E`), `"Promise Not Yet Defined
1.1098031612"` in the drivers; the TX4 and TX2000 drivers keep the identical
format, so arrays are portable across generations. RAID level names are fixed
15-character strings ("+0 Span        ", "X2 Mirror/RAID1", "+0 Stripe/RAID0").
[R][W]

Location: the start of the last cylinder or track (formula disagreement in
section 14). The firmware issues IDENTIFY and reads these four sectors on every
attached disk at POST, and writes them on array create/delete; nothing else is
read at POST outside the INT 13h boot path. [R][W]

### 11.6 Boot-time interface

Banner, "Scanning IDE drives ", per-channel list with "Mode (P = PIO, D = DMA,
U = UDMA)", the array table ("ID     MODE               SIZE     TRACK-MAPPING
STATUS") or "No Array is defined...", warnings (the blocking "WARNING! The array
is in CRITICAL state. ... or Press ENTER key to continue to boot.", the cable
warning, the two-controller warning, the IRQ warning), then "Press <Ctrl-F> to
enter FastBuild (tm) Utility...". The keyboard buffer is flushed, then 5000
iterations of a ~1 ms delay poll INT 16h; **AX = 0x2106 (Ctrl-F)** enters
FastBuild, ESC is AX = 0x011B (`ft100B24.bin 0xAAAF`, `bios.bin 0xB046`).
FastBuild 1.32 (c) 1996-2001: Auto Setup, View Drive Assignments, Define Array,
Delete Array, Rebuild Array, Controller Configuration; Span / Stripe / Mirror /
Mirror/Stripe; stripe blocks 0.5-1024 KB. Its "[ System Resources Configuration
]" screen shows the IRQ and I/O port it resolved, a convenient first check that
an emulated card is seen correctly. The TX2000 adds "No Disk is detected on
Promise Controller. / System will continue to boot ..." (0xB1D8). [R]

---

## 12. Emulation notes

These are the behaviours an emulation must get right for Promise's own firmware
and drivers to work, gathered from sections 4-11 and from running the real
binaries against the 86Box model (PDC20269 / PDC20271, [E]).

### 12.1 PCI identity

1. Device ID, class and subclass must match per part (01:04 for the PDC20246
   and every FastTrak, 01:80 for the other Ultra parts). A mismatch is fatal in
   the ROMs and in the Windows drivers. [R][W]
2. Subsystem 105A:4D33 for a retail Ultra33/66/100; 105A:4D32 for the
   FastTrak100 ROM (4D39 for its INF); the TX2 ROMs do not care. The 86Box model
   sets the subsystem to 105A and the device ID (4D69 / 6269). [R][W][E]
3. Revision >= 2 if the Ultra66 2.00.18 ROM should select UDMA5. The model
   uses revision 0x02, prog-if 0x8F, status 0x0280, interrupt pin INTA,
   Min_Gnt 0x04, Max_Lat 0x12. [R][E]
4. Four I/O BARs with bit 0 set plus BAR4; on the PDC20269 also BAR5 (16 KiB
   memory). Offer BAR5 wherever `ultra.sys` will look for it; `fasttrak.sys`
   ignores it, and the model omits it on the FastTrak. [R][W][E]
5. The expansion ROM BAR must size to 16 KiB or 64 KiB with read-only low bits
   (section 4.5). [E]
6. **Answer configuration cycles on function 0 only.** Answering on every
   function made the BIOS find a second controller and run the option ROM
   twice. [E] (`fasttrak.sys` also counts function 0 only through 0xCF8.) [W]
7. Configuration 0x40-0x42, 0x50-0x51 and 0x60-0x7F must accept writes and read
   them back; drivers written for the whole family write the old-generation
   registers without asking which part they have. The model answers config
   0x50 bits 10/11 consistently with indexed 0x0B bit 2 even on the TX2 part.
   [W][E]

### 12.2 Bus-master block

8. Decode the index/data pair at +1/+3 and +9/+0x0B before the SFF core on TX2
   parts. [L]
9. **The bus-master start bit must drop by itself at the end of the PRD list**
   (section 8.2). [W][E]
10. **Mask the SFF status "active" bit (bit 0) whenever command bit 0 is
    clear.** The 86Box SFF core comes out of reset with the active bit up, and
    the TX2000 ROM refuses to start a transfer on a channel that says one is
    already running (`bios.bin 0x2D33`). [R][E]
11. BAR4+0x1D (old parts, and harmless to provide on TX2 parts): bits 2/6 must
    track BM status bit 2 of each channel; report FIFO empty (0x11) since
    firmware that waits for an empty FIFO otherwise waits forever. [R][L][E]
12. BAR4+0x11, 0x1A-0x1C, 0x1E, 0x1F and the 32-bit 0x20/0x24 must store and
    return what was written; none needs an effect. [R][W]
13. BM status bit 2 must be set with every interrupt; the bus-master status and
    command registers must read back for read-modify-write. [R][W]

### 12.3 Indexed registers and PLL

14. Indexed 0x0B: bit 2 per cable (set = 40-conductor); **never** let
    `(value & 0xA0) == 0x20`. The model masks 0xA4 out of the stored value and
    supplies bit 2 from the cable setting (default 80-conductor). [W][E]
15. Timing registers 0x0C-0x1B and the PLL pair need no effect (nothing in an
    emulator counts ATA cycles), but must read back what was written. [W][E]
16. **The PLL pair survives reset.** The model keeps indexed 0x02/0x03 across
    reset and, if nothing has programmed them (a boot without the card's ROM),
    presets R = 8, F = 78, what the ROM would leave for 133 MHz from a
    16.667 MHz input. [W][E]
17. **The PLL counter**, with semantics that satisfy both the ROM and Linux:
    - it counts down at the modelled input (the model uses 16 666 667 Hz) in
      **emulated** time, because the ROM times its 36-tick interval with the
      emulated BIOS tick; [R][E]
    - it reads 0x3FFFFFFF the first time test mode is entered after reset;
      [R]
    - **test mode is a gate, not a reset**: on 0x01 bit 6 going 1 → 0 the count
      is latched and held; on 0 → 1 it resumes from the held value. The ROM
      reads only after stopping, so a counter that returned to the top on stop
      reports zero elapsed time and the ROM divides by zero. A counter that
      restarted from the top on every start made a driver that samples twice
      compute a wrapped difference of almost the whole 30 bits, a clock of about
      100 GHz. [R][L][E]
    - split as 8/7/8/7 bits across the two pairs (section 6.1). [L][R]
    - Unit pitfall found in the model: converting the CPU cycle count to
      microseconds must use the integer part of 86Box's `TIMER_USEC`, which is
      a 32.32 fixed-point value; dividing by the whole of it gives zero, and a
      zero count is exactly what the ROM then divides by. [E]
    - Keep the apparent input between 5 and 25 MHz to stay clear of the ROM's
      16-bit divide. [R]

### 12.4 Channels and interrupts

18. After SRST, alternate status must settle to exactly 0x50 (or 0x00 for an
    empty channel) or `ultra.sys` waits five seconds per channel; `fasttrak.sys`
    accepts 0x50-0x53. Absent devices must read 0xFF. [W]
19. Tolerate indexed 0x01 bits 3:2 cleared and set, BAR4+0x1F bit 4 pulsed, and
    a device-select flip mid-transfer, without losing drive presence. [W][R]
20. **SFF slot and IRQ setup belong in reset, not init.** In 86Box, adding the
    card only queues it; the PCI slot is assigned later when cards are
    registered. A bus master told its slot at init is told an invalid one, and
    an interrupt raised from an invalid slot is silently dropped. Reset runs
    after registration, so the model sets slot, IRQ pin (INTA) and IRQ mode
    there. [E]
21. The BAR5 memory window must be a second view of the same task file, alternate
    status and bus master, not separate state. [W]
22. The ROMs issue ordinary ATA reads of four sectors at the reserved-sector LBA
    on every disk (FastTrak); disk images need nothing special unless arrays are
    wanted. [R][W]

---

## 13. Open questions and unverified claims

None of these blocks an emulation; each is a place where a model is guessing.

1. **BAR4+0x1A / +0x1B** (old parts): written 1 before and 0 after each transfer
   by the Ultra66 ROM, 0x01 once at init by the drivers and the FastTrak100 ROM,
   only ever read (and printed) by Linux. Per-channel enable, status clear or
   FIFO reset? [R][W][L]
2. **BAR4+0x1C bits 0-1** (gen 3) and **BAR4+0x1E** (FastTrak100 ROM, written
   0). Unknown. [W][R]
3. **BAR4+0x1F bit 0** ("UDMA speed flag") is set by everything and read by
   nothing; whether clearing it disables UDMA is untested. **Bits 7:6**, the
   latter fed from config 0x50 bit 6 on gen 3, are a two-bit field of unknown
   meaning. [L][R][W]
4. **Config word 0x50 bit 6**: a strap or board bit, consumed only by item 3.
   [W][R]
5. **Config 0x40 (word written 0) and 0x42 bit 0 (cleared)**: undocumented;
   possibly one Promise-private dword that changed between generations [?].
   [R][W]
6. **Config 0x30 & 0xC000** read by both FastTrak drivers and never used. [W]
7. **BAR4+0x1D bits 1/3/5/7** (FIFO full, error) are never tested by any ROM, so
   their behaviour is unconstrained. Whether +0x1D exists on the TX2 parts is
   untested. [R][W]
8. **Indexed 0x0B bits 7 and 5**: named only by inference; bit 5's role in the
   Ultra133 TX2 ROM's slave-ATAPI path is unknown. [W][R]
9. **Indexed 0x13 bit 1**: FIFO/prefetch enable by analogy only. [W]
10. **Indexed 0x10 bit 7**: a hold-time bit by inference only. [?]
11. **Indexed 0x02/0x03 on the primary pair**: whether they decode there
    (separately, or as an alias) is untested; every source writes them through
    the secondary pair. [R][L][W]
12. **Index space vs MMIO 0x1102/0x1202** (section 5.4). [L][R]
13. **The counter's true reset behaviour**: the ROM requires 0x3FFFFFFF at the
    start of test mode; Linux samples first and is indifferent. A real part must
    either reset the counter on the 0 → 1 edge or hold it at 0x3FFFFFFF while
    test mode is off. (The 86Box model resumes from the held value; that
    satisfies the ROM only because the ROM measures once per reset.) [R][L][E]
14. **BAR5 task-file stride** 0x0A, 0x0F, 0x10, 0x15, 0x1A, 0x1F (gaps 5, 1, 5,
    5, 5), and why channel 1 sits 0x200 below channel 0. Unambiguous in the code,
    not understood. [W]
15. **Whether the PDC20268 has a BAR5.** [W][L]
16. **Why `fasttrak.sys` ignores BAR5** on the silicon `ultra.sys` uses it on
    [?: a single-drive latency optimisation not worth porting into the RAID
    engine].
17. **Old-family timing byte A for the slave**: `fasttrak.sys` masks it to bits
    1:0; `ultra.sys` writes all eight bits, Linux/CORSAC preserve them. Nothing
    breaks either way [?: the upper bits are ignored on the slave's register].
    [W][L][C]
18. **Why the vendor ORs 0xC0 into old-family PIO byte A** when Linux preserves
    those bits. [W][L]
19. **Config 0x62/0x6A on TX2 parts**, read live by the TX2000 ROM; whether the
    PDC20271 decodes the old timing block at all. Return 0. [R]
20. **`out BAR4+1, 0x00`** in the TX2000 ROM, purpose unknown. [R]
21. **PCIR prog-if** 0x8F (Ultra, TX2000) vs 0xD8 (FastTrak100); nothing reads
    it; what the real chips report at config 0x09 is untested. [R]
22. **Header bytes 0x20-0x27** of the TX2 ROMs (`69 4D 5A 10 68 4D 20 00`,
    `69 62 5A 10 68 4D 29 00`) and the `"PROMISE"` tag at 0x2A, assumed to be
    `PTIFLASH.EXE` metadata; the second ID 0x4D68 in the TX2000 header is odd.
    [R][?]
23. **The `0x6268` guards** in the FastTrak100 and TX2000 ROMs: intentional
    per-product switches or build slips. A PDC20270 ROM would settle it (compare
    its 0x0F12 constant). [R]
24. **The Ultra66 PnP device id** `89 42 66 A1` decodes as "PTI" but the product
    nibble order was not verified. [R]
25. **Ultra66 `[cs:si+0x3F]`** (from IDENTIFY word 82 bits 0 and 10) is written
    but never branched on; the purpose of the `'Y'`/`'N'` PnP flag beyond
    suppressing the INT 13h hook was not traced; VDS use under a V86 manager was
    not analysed. [R]
26. **`[cs:0x3D6]` bits 0x10/0x20** in the Ultra133 TX2 ROM disable DMA per
    controller but are never set by the ROM [?: patched by a utility]. [R]
27. **The Ultra66 v1.14 flash image** declares 0x80 blocks (64 KiB) for 16 KiB
    of code [?: the remainder is padding]. [R]
28. **PDC20276/20277** (5275/7275) are probed by `fasttrak.sys` but absent from
    every INF; believed to be the same silicon as the 6269 [?]. [W]
29. **The FastTrak100 ROM's reserved-sector multiply** was not located; assumed
    identical to the drivers' [?]. The metadata bytes 0x01C-0x1FF were not
    decoded. [R]
30. **Whether an Ultra100 ROM of the ft100B24 vintage** uses B10E like the
    Ultra66 or B10F like the FastTrak. [R]
31. **The INT 40h loop** (section 9.4) is an observed interaction; the specific
    second ROM was not traced. [?]
32. **The SuperSwap / Winbond enclosure routines** were traced far enough to
    show they use the task file, not a Promise register block. [W]

---

## 14. Where the sources disagree

1. **BAR4+0x20 direction bits.** Linux: read = 0x05000000, write = 0x06000000
   (read bit 24, write bit 25). `fasttrak.sys` 2.00 b33 has separate read and
   write code paths and sets bit 24 on read, bit 25 on write. The `ultra.sys`
   analysis reads the same code the same way (the flag tested, 0x40, is
   `SRB_FLAGS_DATA_IN` in the Windows DDK). The `ultra.mpd` analysis read 0x40
   as `SRB_FLAGS_DATA_OUT` and so concluded the vendor is the reverse of Linux,
   which the earlier synthesized reference repeated as an open question; given
   the DDK value and `fasttrak.sys`, three sources agree on **bit 24 = read, bit
   25 = write, bit 26 = UltraDMA**. The FastTrak100 option ROM analysis records
   `(count<<8) | 0x02000000` for read and `| 0x04000000` for write, which fits
   neither; those constants should be re-derived. A model should take the true
   direction from the ATA command. [L][W][R]
2. **Indexed 0x13 bit 1 polarity.** The `ultra.sys` analysis says it is set for
   ATA (non-ATAPI) devices; the `ultra.mpd` analysis of the identical code says
   ATAPI (while describing the same flag bit as "not ATAPI" in its own old-family
   section); the earlier synthesized reference says ATAPI. `fasttrak.sys` sets it
   unconditionally and attaches only disks. The weight of evidence is "set for
   ATA disks". [W]
3. **When `ultra.*` 2.00.43 writes the TX2 timing tables.** The `ultra.sys`
   (WinXP) analysis finds them written only on generation 5 with a UDMA6 drive
   (`ext+0xB8F`), never on the PDC20268; the `ultra.mpd` analysis states they are
   programmed on types 4 and 5 identically, with no 100 MHz special case. The two
   binaries share a source tree and build date; this was not reconciled.
   `fasttrak.sys` and both TX2 ROMs follow the "only at 133 MHz" policy. [W][R]
4. **Reserved-sector LBA.** The FastTrak100 analysis renders the driver
   arithmetic as `spt × heads × (cylinders − 1)`, the first sector of the last
   cylinder; the TX2000 ROM analysis reads its code (`bios.bin 0x15BD-0x15C7`) as
   `spt × (heads × cylinders − 1)`, the first sector of the last track, argues
   the driver's `imul/dec/imul` sequence is the same, and notes a second variant
   using `cylinders − 1` (`dec dword [si+0x14]` at 0x1593). Not reconciled; the
   magic string at offset 0 is the reliable way to find the block. [W][R]
5. **PCIR prog-if of the Ultra133 TX2 ROM.** Its analysis decodes the class
   bytes as prog-if 0x00, and the TX2000 analysis repeats that; the raw bytes it
   quotes (`00 00 18 00 00 8f 80 01` at 0x2100) give revision 0, then class code
   `8F 80 01`, i.e. prog-if **0x8F**, the same as the Ultra66 ROM and the earlier
   synthesized reference. [R]
6. **Whether the Ultra66 ROM has a PCIR.** The FastTrak100 analysis states the
   Ultra66 ROM's pointer at 0x18 "does not point at a PCIR"; the Ultra66
   analysis shows `PCIR` at 0x183F, exactly where 0x18 points, with the runtime
   flag byte at 0x183E. The Ultra66 analysis is correct. [R]
7. **Half the PCI clock.** The ROM analyses and the 86Box model use 16.667 MHz
   (half of 33.33 MHz); the earlier synthesized reference and a comment in the
   model quote "about 16.949 MHz" / "about 16.9 MHz". Anything in 5-25 MHz
   works for the ROMs. [R][L][E]
8. **Which clock the counter runs against.** The earlier synthesized reference
   says the counter must decrement "in terms of host elapsed time, because the
   driver times the interval with the host clock"; for an emulation the interval
   is measured by the guest's own (emulated) timer, and the model derives the
   count from emulated time. [E]
9. **Subsystem device of the FastTrak100**: 0x4D32 (ROM) vs 0x4D39 (INF),
   section 11.4. [R][W]
10. **Class test on the PDC20246 in FastTrak drivers**: the FT100 Win2000
    `FASTTRAK.SYS` exempts 4D33 from the class test; `fasttrak.sys` 2.00 b33
    requires 01:04 for it on the NT4 scan path; `ultra.sys` requires 01:04 for
    it too. Consistent with the PDC20246 always being 01:04; noted because the
    exemption exists. [W]
