# arm64 FEAT_NMI / pseudo-NMI test suite

Repository: <https://github.com/ruanjinjie-eng/feat_nmi_tests>

Tests for the arm64 NMI support series (`feat_nmi_v1` in `../linux_old1`),
run on QEMU (11.1.50 with FEAT_NMI/FEAT_GICv3_NMI/GICv5 emulation) or on
real hardware (see "Running on real hardware" below).  The tree is
self-contained except for the kernel source and QEMU, which default to
`../linux_old1` and `../qemu` (override with `KSRC=` and `QEMU=`).

## What is covered

Boot-time capability matrix plus functional delivery tests:

| # | Case | CPU | GIC | Kernel cmdline | Expected |
|---|------|-----|-----|----------------|----------|
| 01 | `no_nmi` | neoverse-n1 (no FEAT_NMI) | GICv3, no NMI | - | no NMI at all |
| 02 | `pseudo_nmi` | neoverse-n1 | GICv3, no NMI | `irqchip.gicv3_pseudo_nmi=1` | pseudo-NMI via priority masking |
| 03 | `arch_nmi` | max (FEAT_NMI) | GICv3 with FEAT_GICv3_NMI | - | architected NMI |
| 04 | `pe_nmi_gicv2` | max | GICv2 (no NMI) | - | PE NMI but no irqchip support; IPI/PMU fall back to IRQ |
| 05 | `gic_nmi_no_pe` | neoverse-n1 | GICv3 forced `has-nmi=on` | - | GIC advertises NMI, PE does not; kernel ignores it |
| 06 | `arch_plus_pseudo` | max | GICv3 with NMI | `irqchip.gicv3_pseudo_nmi=1` | pseudo-NMI wins over architected NMI |
| 07 | `idreg_nmi_off` | max | GICv3 with NMI | `id_aa64pfr1.nmi=0` | PE override: GIC advertises NMI, kernel does not use it |
| 08 | `idreg_off_pseudo` | max | GICv3 with NMI | `id_aa64pfr1.nmi=0 irqchip.gicv3_pseudo_nmi=1` | pseudo-NMI still works |
| 09 | `gicv5_arch_nmi` | max + `has_gcie=on` | GICv5 (`gic-version=x-5`) | - | architected NMI, LPI-backed IPIs |
| 10 | `gicv5_pseudo_arg` | max + `has_gcie=on` | GICv5 | `irqchip.gicv3_pseudo_nmi=1` | pseudo cannot be used on GICv5; arch NMI stays |
| 11 | `gicv4_arch_nmi` | max | GICv4 (`virtualization=on`) | - | architected NMI over GICv4 |
| 12 | `pseudo_on_gicv2` | neoverse-n1 | GICv2 | `irqchip.gicv3_pseudo_nmi=1` | pseudo cannot be enabled without GICv3 CPUIF |
| 13 | `arch_stress_hotplug` | max | GICv3 | `nmi_extra=sysrq,perf,stress,hotplug nmi_runs=3` | concurrency/perf stress + CPU hotplug retest |
| 14 | `pseudo_hardlockup` | neoverse-n1 | GICv3 | pseudo + `nmi_extra=perf,hardlockup` | perf/NMI hardlockup watchdog fires |
| 15 | `arch_hardlockup` | max | GICv3 | `nmi_extra=perf,hardlockup` | perf/NMI hardlockup watchdog fires |
| 16 | `no_nmi_stress` | neoverse-n1 | GICv3 | `nmi_extra=perf,stress` | stress path without NMI (fallback) |
| 17 | `pseudo_stress` | neoverse-n1 | GICv3 | pseudo + `nmi_extra=perf,stress` | stress path with pseudo-NMI |
| 18 | `arch_suspend` | max | GICv3 | `nmi_extra=suspend` | suspend/resume, probe retest after wakeup |
| 19 | `uefi_acpi_arch` | max | GICv3 | `-bios QEMU_EFI.fd`, `nmi_extra=perf,efi` | ACPI boot + EFI runtime wrappers |
| 20 | `uefi_acpi_pseudo` | neoverse-n1 | GICv3 | UEFI, pseudo + `nmi_extra=perf,efi` | ACPI + pseudo-NMI + EFI |

PE support is selected by the CPU model (`max` has FEAT_NMI,
`neoverse-n1` does not) or the `id_aa64pfr1.nmi=` override.  GIC support is
selected by the GIC version and `arm-gicv3.has-nmi`.  Note that QEMU's
`virt` machine sets `has-nmi` itself when the CPU has FEAT_NMI, so
`-global arm-gicv3.has-nmi=off -cpu max` cannot produce "PE NMI, GICv3
without NMI"; GICv2 covers that combination.

"GIC NMI but PE without NMI" is not an architecturally valid combination,
so it is simulated (forced GIC property and the kernel idreg override) to
check that the kernel ignores the GIC capability.

## How the checks work

The guest init collects:

1. **Capability and delivery probe** (`module/nmi_probe.c`):
   - `system_supports_nmi()` / `system_uses_nmi()` /
     `system_uses_irq_prio_masking()`.
   - Single-target latency: a helper thread on another CPU disables local
     IRQs for 1.5 s; the sender calls `cpumask_backtrace()`.  A real NMI
     is delivered during that window (the call returns in a few ms), a
     plain IRQ is only serviced when the helper re-enables IRQs (~1.5 s).
   - Broadcast latency: helpers on up to `bc_max` CPUs hold IRQs off and
     one backtrace IPI is broadcast to all of them, testing concurrent NMI
     delivery (same path as sysrq-l).
2. **Kernel log**: GIC NMI capability, pseudo-NMI setup, pseudo-vs-arch
   precedence, and `arm-pmu ... using NMIs`.
3. **irq debugfs**: IRQ descriptors with `IRQS_NMI`, classified by hwirq
   (SGI = IPI, PPI = PMU).
4. **Exception-state and kprobe tests** (always run):
   - `irqflags`: DAIF/PMR/ALLINT save, restore and nesting;
     `arch_irqs_disabled()` must consider ALLINT; `local_irq_disable()`
     must leave ALLINT clear (NMIs stay unmasked) with FEAT_NMI; pseudo
     masking must use PMR.  With `CONFIG_DEBUG_IRQFLAGS` the arch code
     validates the whole DAIF/PMR/ALLINT combination on every operation.
   - `allint_mask`: a helper sets ALLINT (DAIF clear) and a backtrace IPI
     is sent; the NMI must stay pending until ALLINT is cleared
     (~1.5 s), proving architected NMI masking.
   - `kprobe`: probing `vfs_read` must work and hit; probing an MSR ALLINT
     (`nmi_enable`) or an MRS ALLINT instruction must be rejected
     (`-EINVAL`), covering "arm64: probes: Reject stepping over ALLINT".
   - `randomness`: a kprobe on `add_interrupt_randomness` counts calls made
     in NMI context; it must stay 0, covering the genirq NMI change.
5. **Optional tests** (`nmi_extra=`):
   - `sysrq`: sysrq-l cross-CPU backtrace.
   - `perf`: userspace `perf_event_open()` cycles sampling; checks the
     cycle count and the arm-pmu IRQ count.
   - `stress`: system-wide perf sampling on every online CPU while another
     process floods sysrq-l (concurrent NMIs + cross-CPU backtraces).
   - `hotplug`: offline/online CPU1, then rerun the probe (per-CPU NMI
     enable/disable paths).
   - `suspend`: RTC-wakeup suspend/resume, then rerun the probe
     (`PSTATE.ALLINT` restore).
   - `hardlockup`: enables the perf hardlockup detector, runs the upstream
     `lib/test_lockup.ko` with `disable_irq=1` and checks
     `/sys/kernel/hardlockup_count` / "Watchdog detected hard LOCKUP".
   - `efi`: mounts efivarfs and reads a variable 64 times through the
     runtime wrappers (checks the DAIF/ALLINT corruption warning).
   - `crash`: leaves a CPU spinning with IRQs disabled and panics, forcing
     `smp_send_stop()` onto the `IPI_CPU_STOP_NMI` rung (run separately with
     `scripts/run_crash_test.sh`; it does not return).

Any `WARNING:`, `kernel BUG`, `Oops`, `Unable to handle kernel` in dmesg
fails the case.

`nmi_expect=auto` derives the expected results from the detected
capabilities; this is what the hardware runner uses.  Internal consistency
checks also run in explicit mode:

- architected and pseudo NMI are never both active;
- with no effective NMI the IPI must not be delivered with IRQs off, and
  no IRQ may have `IRQS_NMI`;
- with effective NMI the IPI/broadcast must be fast and `IRQS_NMI` must be
  present;
- the arm-pmu NMI registration must agree with NMI being effective.

"Effective" means an NMI mode is enabled *and* the irqchip registered NMI
IRQs.  PE FEAT_NMI with an irqchip that cannot deliver NMIs is reported as
`nmi_fallback=1` (this is intended behaviour, see case 04).

## Build and run (QEMU)

```bash
git clone https://github.com/ruanjinjie-eng/feat_nmi_tests.git
cd feat_nmi_tests
./scripts/build_kernel.sh      # out-of-tree build in build/linux
./scripts/build_guest.sh       # module + static init + initramfs
./scripts/run_tests.sh         # 20-case matrix, logs under logs/<ts>/

./scripts/run_crash_test.sh    # panic / IPI_CPU_STOP_NMI cases (4)
./scripts/run_config_variants.sh   # ARM64_NMI=n and ARM64_PSEUDO_NMI=n builds
```

Requirements: `aarch64-linux-gnu-gcc` with static libc, a QEMU with
FEAT_NMI support (default `../qemu/build/qemu-system-aarch64`), the kernel
source (default `../linux_old1`).  The UEFI/ACPI cases need an AArch64
firmware image (default `/usr/share/qemu-efi-aarch64/QEMU_EFI.fd`).
Override with `QEMU=`, `KSRC=`, `KDEFCONFIG=`, `JOBS=`.

The kernel is built with `CONFIG_DEBUG_IRQFLAGS=y` (the series' own
runtime DAIF/PMR/ALLINT state checks) and `CONFIG_KPROBES=y` (ALLINT
instruction probing tests).  `run_config_variants.sh` additionally builds
`CONFIG_ARM64_NMI=n` and `CONFIG_ARM64_PSEUDO_NMI=n` kernels in separate
build directories and boots the `auto` checks against them.

The latest results (kernel `53b5c789c1e7`, QEMU 11.1.50: 20/20 matrix,
4/4 panic-path, 5/5 Kconfig variants) and all findings are recorded in
[`RESULTS.md`](RESULTS.md).

Per-case serial logs, `summary.txt` and the observed values
(`NMI_TEST: ...` lines) are stored under `logs/<timestamp>/`.

## Running on real hardware

The same initramfs works on hardware; only the kernel configuration and
the boot method differ.

### 1. Build a kernel for the board

```bash
cd feat_nmi_tests
KDEFCONFIG=defconfig ./scripts/build_kernel.sh
```

For a real board use the platform defconfig (`KDEFCONFIG=<vendor>_defconfig`)
so the serial console, storage and other board drivers are present.  The
script merges `configs/nmi.config` on top; the required options are:

```
CONFIG_ARM64_NMI=y
CONFIG_ARM64_PSEUDO_NMI=y       # only needed to test pseudo-NMIs
CONFIG_GENERIC_IRQ_DEBUGFS=y
CONFIG_MODULES=y
CONFIG_SMP=y
CONFIG_MAGIC_SYSRQ=y
CONFIG_PERF_EVENTS=y
CONFIG_ARM_PMU=y
CONFIG_SOFTLOCKUP_DETECTOR=y
CONFIG_HARDLOCKUP_DETECTOR=y    # perf/NMI watchdog
CONFIG_TEST_LOCKUP=m            # upstream hardlockup test module
CONFIG_DEBUG_IRQFLAGS=y         # series' DAIF/PMR/ALLINT state checks
CONFIG_KPROBES=y                # ALLINT instruction probing tests
CONFIG_PRINTK_TIME=y
CONFIG_IKCONFIG_PROC=y          # optional, for /proc/config.gz
```

If the board enforces module signature verification, either disable
`CONFIG_MODULE_SIG_FORCE` for the test kernel or sign `nmi_probe.ko` and
`test_lockup.ko`.

### 2. Boot the test kernel

`scripts/run_hardware.sh` builds everything and prints the recipes:

```bash
CONSOLE=ttyAMA0,115200 EXTRA=perf,sysrq ./scripts/run_hardware.sh --print
```

* **kexec** (run on the target):

  ```bash
  kexec -l Image --initrd=initramfs.cpio.gz \
        --command-line="console=ttyAMA0,115200 earlycon log_buf_len=2M \
        nmi_expect=auto nmi_extra=perf,sysrq nmi_runs=3 nmi_shutdown=halt"
  kexec -e
  ```

  The script can also do this: `./scripts/run_hardware.sh --kexec`.

* **GRUB / U-Boot / PXE**: copy `Image` and `initramfs.cpio.gz` to the boot
  medium and add the same command line.  Recipes are printed by
  `--print`.

Set `nmi_shutdown=halt` (default in the hardware script) or `reboot`;
do not use `poweroff` unless you want the board to power off.

### 3. What to look for

The console shows lines such as:

```
CPU features: detected: Non-maskable Interrupts
GICv3: Non-maskable interrupt property is supported
hw perfevents: enabled with armv8_pmuv3 PMU driver, 7 counters available, using NMIs
NMI_TEST: supports=1 active=1 pseudo=0 ...
NMI_TEST: nmi_irqs=4 ipi_nmi=3 ppi_nmi=1 ...
NMI_TEST: fast=1 bc_fast=1 ...
NMI_TEST: RESULT PASS
```

The last line is authoritative.  Expected platform flavours:

| Platform | supports | active | pseudo | fast | nmi_irqs |
|----------|----------|--------|--------|------|----------|
| no NMI, no pseudo | 0 | 0 | 0 | 0 | 0 |
| no FEAT_NMI + `irqchip.gicv3_pseudo_nmi=1` | 0 | 0 | 1 | 1 | >0 |
| FEAT_NMI + FEAT_GICv3_NMI | 1 | 1 | 0 | 1 | >0 |
| FEAT_NMI, GIC without NMI | 1 | 1 | 0 | 0 | 0 (`nmi_fallback=1`) |

`nmi_extra=all` additionally runs hotplug, suspend, EFI and the
hardlockup watchdog (not `crash`).  The hardlockup test deliberately locks
one CPU with IRQs disabled for ~14 s; `hardlockup_panic` is forced to 0 and
the softlockup detector is disabled for the duration.  On hardware the
detection time depends on the PMU frequency (`max_cpu_freq *
watchdog_thresh` cycle period).

For the panic path run `scripts/run_crash_test.sh`: the guest arms a CPU
with IRQs off, then panics.  With NMI support the log must contain
`SMP: retry stop with NMI for CPUs ...`; without any NMI the kernel logs
`SMP: failed to stop secondary CPUs`.  Use a test machine.

External NMI sources cannot be requested from userspace.  Generic
in-kernel NMI users are the IPI backtrace path (also sysrq-l), the arm_pmu
perf IRQ (also the hardlockup watchdog), SDEI and kgdb.  This suite covers
the first two; vendor SDEI/SPI NMI users need their own test hooks.

Firmware/boot requirements: the series documents that when the kernel is
entered at EL1 with EL2 present, the bootloader must leave
`HCRX_EL2.TALLINT` clear, and `PSTATE.ALLINT` must be clear on entry.  A
platform that violates this can fail very early (before any test runs);
check the early boot log if the board does not reach the guest init.

For ACPI platforms the same image works (the drivers use the generic IRQ
domain and capability detection); `efi` additionally reads a runtime EFI
variable.  For DT platforms no board-specific data is needed.

## Existing tests in the wild

A survey before writing this suite (2026-10):

- **kvm-unit-tests**: no arm64 NMI/pseudo-NMI tests.  The old
  `git.kernel.org` repository is abandoned; the current tree at
  `gitlab.com/kvm-unit-tests/kvm-unit-tests` was cloned and searched
  (`arm/`, `lib/arm*`) — no NMI/superpriority tests.
- **Linux selftests**: nothing for arm64 NMI
  (`tools/testing/selftests/arm64/`); the x86 selftests are x86-specific.
- The **series cover letter** states that most testing was done on QEMU,
  Fast Model and pre-silicon, and asks for real-hardware testing — no test
  module is shipped with the series.
- Reusable in-tree utilities: `lib/test_lockup.c` (hardlockup / perf NMI
  watchdog, used here), sysrq-l (`arch_trigger_cpumask_backtrace()`) and
  perf events.  This suite combines them into an automated matrix.

## Limitations

- QEMU GICv5 is experimental and needs `-cpu max,has_gcie=on`; GICv4 in
  TCG needs `-machine virt,virtualization=on`.
- KVM guest NMI, real SDEI event delivery, and vendor SPI NMI users are not
  exercised.  ACPI boot and EFI runtime wrappers are covered by the UEFI
  cases (19/20) when an AArch64 firmware image is available.
- The panic/`IPI_CPU_STOP_NMI` path is destructive (it panics); run
  `run_crash_test.sh` on a test machine only.
- `nmi_extra=suspend` depends on a working RTC wakeup source.  Case 18
  covers it on QEMU; on hardware verify that the board's RTC can wake the
  system from suspend before relying on it.

## License

Unless stated otherwise, the code is licensed under GPL-2.0, like the
kernel it tests.
