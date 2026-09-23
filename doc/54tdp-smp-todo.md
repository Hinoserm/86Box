# 54TDP dual-processor: the plan

Goal: the AIR 54TDP running two CPUs, first two P54C Pentiums, then two
Pentium MMX 233s (66 MHz x 3.5) as on the real board. Interpreter and new
dynarec only; the old dynarec is refused for a two-CPU machine and is not
worked on.

Status: research complete (2026-09-23), nothing implemented. Sources: four
code/document research passes over this tree and the Intel manuals in
corsac86 `docs/hardware/cpu/`, plus skiretic's APIC fork
(github.com/skiretic/86box-apic, based on upstream of 2026-05-14).

## The shape of it

Nothing in 86Box can run two CPUs today, and nothing delivers interrupts
through an APIC. Four pieces, in dependency order:

1. **Two CPU contexts in one thread.** Keep every CPU global where it is and
   swap its contents at slice boundaries. Generated code, the interpreter and
   ~2000 call sites keep addressing the same globals; only the scheduler knows
   there are two.
2. **Local APICs, one per CPU, and the APIC bus.** Take skiretic's local
   APIC, which is complete and validated for one CPU and already stores its
   state per APIC; adapt it to the P54C.
3. **The ESC's I/O APIC delivering.** The 82374SB's register file is modelled
   (`intel_pceb_esc.c:440-605`) but delivers nothing. Hook skiretic's I/O APIC
   delivery logic to it.
4. **The board.** Second CPU config, BIOS 1.51, CPUID/APICEN straps, INIT and
   RESET to both sockets, retire the MP-table blanking.

Hardware altered vs added: almost everything is added. The CPU core, memory
and chipsets are altered in small, well-defined places; no device model
changes, and every change is gated on a CPU count above one so single-CPU
machines run the same code as today.

## Facts that shape the design

- **The P54C has a STARTUP IPI.** The Dual processor comes out of RESET or
  INIT waiting for one (241428-005 §3.4.1.4): delivery mode 110b, vector VV,
  starts at CS=VV00h IP=0 in real mode. A STARTUP not preceded by RESET or
  INIT is ignored (Linux sends two). The CMOS 0Fh/40:67 warm-reset method is
  for the external 82489DX only.
- **INIT behaves by socket.** INIT (pin or IPI) sends the Primary to
  FFFFFFF0h keeping caches, FPU, MSRs and SMBASE; it sends the Dual back to
  waiting for STARTUP. The INIT pin is tied to both sockets.
- **CPUID.** The Dual reports type 10b (EAX bits 13:12, value 2xxxh), and
  both set EDX bit 9 when the APIC is hardware-enabled. APIC IDs 0 and 1
  (Dual = Primary ^ 1). Version register 1Xh on the P54C.
- **P54C APIC vs later parts** (241430-004 §19.3.6): NMI and INIT are
  edge-only, ICR interrupts are edge-only except INIT de-assert, logical
  destination 8 bits, APIC ID 4 bits, no APIC base MSR.
- **The Dual ignores A20M# and IGNNE# and never drives FERR#.**
- **The I/O APIC is inside the ESC** (82374SB), 16 pins: pin 0 the 8259
  output (or SMI with PAC bit 1), pin 2 PIT counter 0, the rest their IRQs.
  PAC (index 70h) is the ESC's IMCR. Ports 22h/23h are the ESC's on this
  machine; no conflict with the Cyrix configuration ports.
- **The BIOS finds the second CPU by sending it INIT/STARTUP** and waiting
  for it to check in; there is no presence register. It is in the compressed
  part of the ROM, so the exact sequence is only visible from a traced boot.
  The boot block already sets up virtual wire (LINT0 = ExtINT) and leaves
  LINT1 (NMI) masked.
- **Machine ROM: 1.03 is the uniprocessor build.** A DP boot needs
  `54tdp151.rom` (in the local roms checkout, not yet committed).
- **The 430HX needs no DP awareness.** Two CPUs share one host bus; the
  private snoop bus keeps their L1s coherent, and 86Box models no L1, so
  coherence is free. Reg 50h bit 5 (SP92) is worth making real.
- **NT's MPS HAL needs the I/O APIC delivering.** Virtual wire plus IPIs is
  enough for the BIOS probe and for Linux 2.0-era SMP, not for NT/Win2k,
  Linux 2.2+ or CORSAC's I/O APIC path.

## Stage 1: two CPU contexts

### Decisions

- **Swap by memcpy, fixed addresses.** The new dynarec bases every block on
  `RBP = &cpu_state + 128` (x86-64) / `X29 = &cpu_state` (arm64) and embeds
  `&cpu_state.seg_*`, `ea_seg` and the TLB array addresses. Swapping contents
  keeps every compiled block valid and lets both CPUs share one code cache
  (the block key is physical address, CS base, PC and `cpu_cur_status`).
  Turning state into a pointer would touch thousands of sites and every
  backend for no gain.
- **Switch only at block boundaries,** at the bottom of the 5 µs slice in
  `exec386_dynarec` (`386_dynarec.c:~1026`) and between instructions in
  `exec386`. Never from a callback: an IPI, timer or reset aimed at the other
  CPU writes into its parked context or sets a pending flag.
- **Per-CPU TLBs.** Each CPU gets its own `readlookup2`/`writelookup2`/
  `page_lookup` (24 MB each), with base pointers appended at the end of
  `cpu_state`; the dynarec's four immediate loads (`codegen_backend_x86-64.c`
  :101, :195; `codegen_backend_arm64.c` :100, :168) become loads from
  `cpu_state`. Alternative if memory matters: save and restore the 768 live
  ring entries per switch (no dynarec change, more per-switch cost). Flushing
  per switch is not viable.
- **Time.** CPU 0 drives `tsc` and the timers exactly as today. CPU 1 runs
  its slice with `tsc` frozen and `update_tsc()` a no-op; it keeps its own
  cycle count. Never rewind `tsc`. RDTSC returns `tsc + tsc_offset[cpu]`;
  WRMSR 10h sets the offset and stops calling `timer_set_new_tsc()`.
  (Alternative considered: advance `tsc` by `cycdiff / n` from both CPUs;
  finer device time, more sites to change.)
- **Halted CPUs cost nothing.** Per-CPU run state RUNNING / HALTED /
  WAIT_SIPI / SHUTDOWN. The scheduler skips non-running CPUs; if all are
  halted, jump `tsc` to `timer_target`. OS idle loops HLT, so this is the
  biggest performance lever.
- **Slice length configurable,** 5 µs default. A CPU spinning on a lock the
  other holds burns its slice; the P54C has no PAUSE.
- **Old dynarec refused** for two CPUs (it embeds `readlookup2` in blocks):
  `cpu.c:1907` picks `exec386_dynarec` only with `USE_NEW_DYNAREC` or one CPU.

### What a CPU context holds

- `cpu_state` (~500 bytes: regs, lazy flags, segments, CR0, FPU/MMX, SMM
  fields, `_cycles`). New fields go at the end (arm64 offset limits).
- `fpu_state` (softfloat), Pentium part of `msr`, `static MME[8]`
  (`x86_ops_mmx.c:35`; `MMP/MMEP` point into fixed state, fine under swap).
- About 45 globals: `gdt ldt idt tr cr2 cr3 cr4 dr[] use32 stack32
  cpu_cur_status CPUID nmi nmi_auto_clear nmi_enable new_ne trap cpu_init
  cpu_end_block_after_ins rf_flag_no_clear smi_latched smm_in_hlt smi_block
  old_rammask cpu_old_paging cpu_flush_pending cpl_override oldds/oldss and
  limits`, `cycles_main`, `tsc_old`, `cycles_old`, the Pentium pairing state
  (`codegen_timing_pentium.c:97-111`), `rammask` (full on CPU 1), cache wait
  state values.
- Reset on switch: `pccache`, `get_phys_virt`.
- CPU 1's context must copy CPU 0's host MXCSR fields (`old_fp_control`,
  `trunc_fp_control`), or a zeroed MXCSR unmasks host FP exceptions.
- A debug build should checksum the parked context between slices to catch
  a missed global.

### Files

| File | Change | Size |
|---|---|---|
| `src/cpu/cpu_mp.c` (new), `cpu.h` | contexts, save/restore, scheduler wrapping `cpu_exec` (as gdbstub does), run states, INIT/STARTUP, targeted NMI/SMI | +500-700 |
| `src/cpu/386_dynarec.c` | slice loop per CPU, switch point, `tsc` only on CPU 0, HLT yield, interrupt helpers at the 5 `pic.int_pending` sites | ~80 altered |
| `src/cpu/386.c`, `x86_ops_misc.h:704` | interrupt helper; HLT sets the run state | small |
| `src/cpu/x86.c` | split `reset_common()` into per-CPU RESET, per-CPU INIT (keeps caches/FPU/MSR/SMBASE) and a once-per-event machine reset | ~60 |
| `src/cpu/cpu.c` | DP eligibility, CPUID type and bit 9, TSC offset, old-dynarec gate | ~40 |
| `src/cpu/386_common.c` | `smi_raise`/`nmi_raise` take a target; SMRAM recalc when switching between CPUs whose `in_smm` differs | ~30 |
| `src/cpu/x86_ops_msr.h`, `timer.c:266` | WRMSR 10h sets an offset | small |
| `src/codegen_new/codegen_backend_x86-64.c`, `..._arm64.c` | TLB bases from `cpu_state`; copy host FP control to CPU 1 | ~8 |
| `src/mem/mem.c` | per-CPU TLBs; `flushmmucache*` flushes every CPU (70 callers unchanged), new local flush for CR3/INVLPG; `mem_flush_write_page` scrubs every CPU; bus-master view never SMM | ~200 |
| `src/mem/smram.c` | recalc on switch | ~20 |
| `src/86box.c`, `machine.c` | scheduler hook; `cpu_set()` and `smbase` for both contexts | ~30 |

## Stage 2: local APICs (from skiretic's fork)

The fork has a complete single-CPU local APIC (register file, TPR/PPR, EOI,
ISR/IRR/TMR, LVT, timer, ESR, ICR, physical/logical/lowest-priority
destinations, state helpers) whose state is already arrays sized for several
APICs, an APIC bus, CPUID bit 9 gated by model and board, interrupt
acceptance in the interpreter and dynarec, and INIT/STARTUP bookkeeping that
records the vector but starts nothing. Validated with Linux and Windows 2000
on one CPU.

Work:

- Import the APIC files only (`src/lapic*.c`, `src/apic_bus.*`,
  `src/apic_cpu_policy.c`, `src/apic_platform.*`, the `cpu.c`/`cpu.h`/
  `386_dynarec.c`/`pic.c` hooks). The fork's full diff is ~42,000 lines with
  unrelated work (ARM64 Voodoo JIT, AppImage, CI removal). Credit skiretic;
  talk to him first.
- P54C specifics: version 1Xh, 4-bit IDs, edge-only NMI/INIT/ICR, no APIC
  base MSR, 8-bit logical destinations. The fork defaults to 00040010h.
- The FEE00000 window per CPU: data accesses of the running CPU only; code
  fetches and bus masters go to memory.
- Connect the fork's STARTUP handler to stage 1's run states. EOI and TPR
  writes end the block.

## Stage 3: the ESC's I/O APIC

- Delivery for the 16 pins, edge detection, Remote IRR and level EOI, using
  the fork's I/O APIC logic on the existing ESC register file.
- Pin wiring per ESC Figure 25: pin 0 = 8259 INTR or SMI (PAC bit 1), pin 2
  = PIT 0, pin 8 active low, the rest their IRQs; PCI PIRQs arrive after the
  ESC's own routing.
- PAC bit 0 gates INTR to both CPUs' LINT0. PAC bit 1 sends SMI through the
  APIC (the ESC book says a DP system should).
- Two hooks in `pic.c`: the raw line in `picint_common` (before ELCR
  filtering) and pin 0 in `pic_update_pending_at`. NULL on every other
  machine.
- The ESC raises NMI with `nmi = 1` directly (`intel_pceb_esc.c:382, 421`);
  route through the target-aware raise.

## Stage 4: the board

- `m_at_eisa_pci.c`: a "Processors" setting (1/2) and a BIOS choice
  (1.03 / 1.51) in `at_54tdp_config`; APICEN strapped on; skip
  `ioapic_ami_device` when stage 3 delivers (never before: a visible MP table
  with no delivery breaks even a one-CPU Win2k install).
- Commit `54tdp151.rom` to the roms fork.
- INIT to both sockets from the KBC reset line and port 92 (`kbc_at.c:802`,
  `port_92.c:80`); hard reset to both (`kbc_at.c:834, 2596`, `pci.c:407`
  CF9, `86box.c:1914`); triple fault goes through the 430HX SP92 path.
- `intel_4x0.c`: reg 50h write mask 0xfd (bit 3 R/W, bit 1 reserved), SP92
  behaviour.
- DP eligibility: two CPUs only for Intel P54C and Pentium MMX (desktop),
  same speed; one family/speed setting plus a count.
- Later, generic: `MACHINE_DUAL_CPU` flag, a count in the machine settings,
  VM Manager display, gdbstub threads (~300 lines total).

## Risks

Inside CPU emulation:

- A per-CPU global missed in the context: silent corruption of the other
  CPU. Mitigation: the debug checksum of the parked context.
- Stale TLB or compiled code across CPUs: a fast write entry in the parked
  CPU's TLB bypassing self-modifying-code tracking. Mitigation: every
  code-page flush scrubs every CPU's TLB.
- The STI/MOV SS shadow or the two-step `cpu_flush_pending` straddling a
  switch: save them, or do not switch while set.
- Both CPUs at the default SMBASE writing one save area if SMI reaches both
  before the BIOS relocates CPU 1.
- INIT today is a full hard reset on one CPU; wrong even for one CPU.
- An existing dynarec flaw (not SMP): PUSHF/PUSHFD check IOPL at compile
  time in V86 mode, but IOPL is not in the block key
  (`codegen_ops_stack.c:388, 406`).

Outside it:

- Single-CPU machines must run identical code: every hook NULL or gated on
  count > 1.
- Device callbacks raising SMI/NMI/INTR from timers land on whichever CPU is
  running today; they need explicit targets.
- Device time resolution while CPU 1 runs (frozen `tsc`): at most one slice.
- Host speed: two 233 MHz CPUs need twice the host work. HLT skipping is the
  main lever.
- gdbstub is single-CPU (register pointers, thread select); GDBSTUB=ON also
  forces the dynarec off, and a two-CPU interpreted Pentium will be slow.
- The fork is four months behind upstream; importing it is a porting job,
  not a merge.

## Validation order

1. Stage 1 with the second CPU parked in WAIT_SIPI: every existing machine
   and the 54TDP with one CPU unchanged.
2. Stage 2: BIOS 1.51 POST with two CPUs, APIC accesses logged: the BIOS
   should STARTUP CPU 1 and report two processors; its MP table lists both.
3. Linux 2.0/2.2 SMP (virtual wire first), then CORSAC.
4. Stage 3: NT4 and Windows 2000 MPS HAL.
5. Two Pentium MMX 233s.

## Open questions

- Which slice length and time model win in practice (5 µs vs 10-50 µs;
  frozen `tsc` vs split advance). Measure.
- Per-CPU TLB arrays (48 MB for two) or ring save/restore.
- The exact BIOS 1.51 probe sequence (trace it).
- Whether skiretic wants to co-own the APIC part upstream.
