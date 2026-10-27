# FEAT_NMI test results

- Date: 2026-10-23
- Kernel: `53b5c789c1e7 irqchip/gic-v5: Add NMI support`
  (`feat_nmi_v1`, v7.3-rc6-38)
- QEMU: 11.1.50 (`<qemu>/build/qemu-system-aarch64`,
  TCG), FEAT_NMI / FEAT_GICv3_NMI / GICv4 / GICv5 (FEAT_GCIE)
- Firmware for ACPI/EFI cases: `/usr/share/qemu-efi-aarch64/QEMU_EFI.fd`
- Kernel config: arm64 defconfig + `configs/nmi.config`
  (`CONFIG_ARM64_NMI`, `CONFIG_ARM64_PSEUDO_NMI`,
  `CONFIG_GENERIC_IRQ_DEBUGFS`, `CONFIG_HARDLOCKUP_DETECTOR`,
  `CONFIG_TEST_LOCKUP=m`, `CONFIG_DEBUG_IRQFLAGS=y`, `CONFIG_KPROBES=y`, ...)
- Results:
  - matrix: `logs/20261023-182747/` — **20/20 PASS**
  - panic / `IPI_CPU_STOP_NMI`: `logs/crash-20261023-183111/` — **4/4 PASS**
  - Kconfig variants: `logs/config-20261023-173026/` — **5/5 PASS**
- No `WARNING:`, `kernel BUG`, `Oops`, `Unable to handle kernel` or
  `Unexpected DAIF` in any non-crash run.

## Matrix

`delta_us` is the time `cpumask_backtrace()` needed to get an ACK from a CPU
spinning with local IRQs disabled (about 1.5 s = plain IRQ, a few ms = NMI).
`bc_delta_us` is the same test broadcasting to all other CPUs.
`nmi_irqs` counts IRQ descriptors with `IRQS_NMI` (SGI-like IPIs + PPIs).
`allint_mask` is the ALLINT-masking test (must be slow: the NMI stays
pending until ALLINT is cleared).

| Case | supports | active | pseudo | nmi_irqs (ipi+ppi) | delta_us | bc_delta_us | allint_mask_us | Result |
|------|----------|--------|--------|--------------------|----------|-------------|----------------|--------|
| 01_no_nmi | 0 | 0 | 0 | 0 | 1501441 | 1501441 | n/a | PASS |
| 02_pseudo_nmi | 0 | 0 | 1 | 4 (3+1) | 2079 | 2079 | n/a | PASS |
| 03_arch_nmi | 1 | 1 | 0 | 4 (3+1) | 1171 | 1171 | 1501512 | PASS |
| 04_pe_nmi_gicv2 | 1 | 1 | 0 | 0 | 1500821 | 1500821 | 1501493 | PASS (fallback) |
| 05_gic_nmi_no_pe | 0 | 0 | 0 | 0 | 1502027 | 1502027 | n/a | PASS |
| 06_arch_plus_pseudo | 1 | 0 | 1 | 4 (3+1) | 1175 | 1175 | n/a | PASS |
| 07_idreg_nmi_off | 0 | 0 | 0 | 0 | 1502576 | 1502576 | n/a | PASS |
| 08_idreg_off_pseudo | 0 | 0 | 1 | 4 (3+1) | 1201 | 1201 | n/a | PASS |
| 09_gicv5_arch_nmi | 1 | 1 | 0 | 7 (6+1) | 1263 | 1263 | 1501622 | PASS |
| 10_gicv5_pseudo_arg | 1 | 1 | 0 | 7 (6+1) | 1222 | 1222 | 1502469 | PASS |
| 11_gicv4_arch_nmi | 1 | 1 | 0 | 4 (3+1) | 1180 | 1180 | 1501788 | PASS |
| 12_pseudo_on_gicv2 | 0 | 0 | 0 | 0 | 1501057 | 1501057 | n/a | PASS |
| 13_arch_stress_hotplug | 1 | 1 | 0 | 4 (3+1) | 1208 | 1208 | 1501622 | PASS |
| 14_pseudo_hardlockup | 0 | 0 | 1 | 4 (3+1) | 1068 | 1068 | n/a | PASS |
| 15_arch_hardlockup | 1 | 1 | 0 | 4 (3+1) | 1253 | 1253 | 1501208 | PASS |
| 16_no_nmi_stress | 0 | 0 | 0 | 0 | 1501316 | 1501316 | n/a | PASS |
| 17_pseudo_stress | 0 | 0 | 1 | 4 (3+1) | 1068 | 1068 | n/a | PASS |
| 18_arch_suspend | 1 | 1 | 0 | 4 (3+1) | 1199 | 1199 | 1501392 | PASS |
| 19_uefi_acpi_arch | 1 | 1 | 0 | 4 (3+1) | 1133 | 1133 | 1501078 | PASS |
| 20_uefi_acpi_pseudo | 0 | 0 | 1 | 4 (3+1) | 1107 | 1107 | n/a | PASS |

All cases also pass the following per-boot checks (no expectation needed):

- `irqflags_ok=1`: save/restore/nesting round trips.
- With FEAT_NMI: `allint_clear_on_disable=1` (local_irq_disable does not mask
  NMIs) and `allint_reported=1` (`arch_irqs_disabled()` sees ALLINT).
- With pseudo-NMI: `pmr_irq_disable=1` (masking uses PMR, not DAIF).
- `kprobe_rc=0`, `kprobe_hits>0` (kprobe path works with ALLINT save/restore).
- `kprobe_msr_reject_rc=-22` and `kprobe_mrs_reject_rc=-22`: probing MSR and
  MRS ALLINT instructions is rejected with `-EINVAL`.
- `random_in_nmi=0`: `add_interrupt_randomness()` is never called in NMI
  context while thousands of NMIs/IRQs are handled.

Optional-test observations:

- 13: `stress_ok=1 stress_pmu_delta=3332 stress_bt=8`, hotplug + retest
  all fast.
- 14/15: `hardlockup=1` (perf/NMI watchdog fired).
- 16/17: stress passes without NMI / with pseudo-NMI.
- 18: `suspend_ok=1 retest_fast=1`.
- 19/20: `efi_ok=1` with 64 EFI runtime variable reads, no
  "DAIF+ALLINT corrupted" warning.

## Panic / IPI_CPU_STOP_NMI

A CPU is left spinning with IRQs disabled, then the kernel panics. The test
checks `smp_send_stop()` behaviour:

| Case | retry with NMI | failed to stop | Result |
|------|----------------|----------------|--------|
| 01_arch_crash | yes | no | PASS |
| 02_pseudo_crash | yes | no | PASS |
| 03_no_nmi_crash | no | yes | PASS |
| 04_gicv5_crash | yes | no | PASS |

This covers `IPI_CPU_STOP_NMI`, `arm64_nmi_cpu_stop()` and the
`local_exceptions_final_mask()` path (also with `CONFIG_DEBUG_IRQFLAGS`).

## Kconfig variants

Separate kernels built with `CONFIG_ARM64_NMI=n` and
`CONFIG_ARM64_PSEUDO_NMI=n`:

| Variant / scenario | supports | active | pseudo | fast | Result |
|--------------------|----------|--------|--------|------|--------|
| ARM64_NMI=n, max, no pseudo | 1 | 0 | 0 | 0 (1.504 s) | PASS |
| ARM64_NMI=n, max, pseudo cmdline | 1 | 0 | 1 | 1 (5.0 ms) | PASS |
| ARM64_NMI=n, neoverse-n1 | 0 | 0 | 0 | 0 | PASS |
| ARM64_PSEUDO_NMI=n, max, pseudo cmdline | 1 | 1 | 0 | 1 (4.6 ms) | PASS |
| ARM64_PSEUDO_NMI=n, neoverse-n1 | 0 | 0 | 0 | 0 | PASS |

The PE capability is still detected with `CONFIG_ARM64_NMI=n`, but the
kernel does not enable NMI delivery; pseudo-NMI remains independently
usable. With `CONFIG_ARM64_PSEUDO_NMI=n` the pseudo option is ignored and
architected NMI is unaffected.

## What was verified

1. **Architected NMI (GICv3/GICv4/GICv5)** works end to end: capability
   detected, GIC property reported, IPIs and the PMU PPI registered as NMI,
   and the backtrace IPI delivered while the target CPU has local IRQs
   disabled (~1.1–1.3 ms vs ~1.5 s for a regular IRQ).
2. **ALLINT masking**: with ALLINT set (DAIF clear), the NMI stays pending
   until ALLINT is cleared (~1.5 s), and `local_irq_disable()` leaves ALLINT
   clear so NMIs remain unmasked. `arch_irqs_disabled()` accounts for
   ALLINT.
3. **Pseudo-NMI** works, uses PMR, and takes precedence when both are
   available; it cannot be enabled on GICv2 or GICv5 (no GICv3 CPU IF).
4. **PE NMI without irqchip NMI (GICv2)**: `system_uses_nmi()` is true but
   IPI/PMU NMI requests fall back to regular IRQs (`nmi_fallback=1`), with no
   WARN/BUG.
5. **GIC advertises NMI but PE does not** (forced GIC property and
   `id_aa64pfr1.nmi=0`): boot cleanly, no NMI used.
6. **Concurrent/broadcast NMI delivery**: one backtrace IPI broadcast to all
   other CPUs' IRQ-off windows is acknowledged in ~1.1–2.1 ms with NMI.
7. **Perf / PMU NMI**: arm_pmu registers the PMU IRQ as NMI; userspace perf
   sampling and all-CPU perf stress with a sysrq-l flood run cleanly.
8. **Perf hardlockup watchdog**: `lib/test_lockup.ko` with `disable_irq=1`
   triggers "Watchdog detected hard LOCKUP" and increments
   `/sys/kernel/hardlockup_count` with architected and pseudo NMIs.
9. **Hotplug and suspend/resume** keep NMI delivery working.
10. **Kprobes**: normal probing works; MSR/MRS ALLINT instructions are
    rejected; the ALLINT save/restore around kprobes is validated by
    `CONFIG_DEBUG_IRQFLAGS`.
11. **genirq NMI change**: `add_interrupt_randomness()` is not called in NMI
    context.
12. **Panic path**: `smp_send_stop()` falls back to `IPI_CPU_STOP_NMI` when
    a CPU is stuck with IRQs off, and reports failure when NMI is
    unavailable.
13. **ACPI + EFI**: UEFI boot with ACPI works with both NMI flavours; EFI
    runtime variable reads do not corrupt DAIF/ALLINT.
14. **Kconfig**: `CONFIG_ARM64_NMI` and `CONFIG_ARM64_PSEUDO_NMI` can be
    independently disabled with the expected behaviour.

## Notes, environment quirks and test-harness issues

These are not kernel defects; they were found and handled while building
the tests:

- `virt` sets `arm-gicv3.has-nmi` itself when the CPU has FEAT_NMI, after
  global properties apply, so `-global arm-gicv3.has-nmi=off -cpu max`
  cannot be used.  GICv2 covers PE-NMI/GIC-without-NMI.
- QEMU GICv4 in TCG needs `-machine virt,gic-version=4,virtualization=on`;
  GICv5 needs `-cpu max,has_gcie=on`.
- ACPI cannot be used with QEMU's direct `-kernel` boot (no EFI/ACPI
  handoff); cases 19/20 boot through EDK2/QEMU_EFI.fd instead.
- The arm64 perf watchdog period is `max_cpu_freq * watchdog_thresh`
  cycles (5 GHz without cpufreq).  Under QEMU TCG (~1 GHz) the test uses
  `watchdog_thresh=1` and a 14 s lockup.
- `/proc/sys/kernel/watchdog` is the master switch; use `soft_watchdog` to
  disable only the soft-lockup detector.
- `test_lockup` returns `-EAGAIN` from module init by design.
- `request_nmi()`/`request_percpu_nmi()` are not exported to modules, so
  the tests use the in-kernel NMI users (IPIs and arm_pmu).
- IRQ debugfs shows `device: (null)` for per-CPU IRQs; NMI IRQs are
  classified by hwirq.
- The kernel command line is newline-terminated; parsing must not NUL the
  buffer (an earlier bug truncated `nmi_extra=`).
- External modules must be cleaned and rebuilt after a kernel config
  change: the stale `.mod.c` kept the old `struct module` size and the
  kernel refused to load it.
- `lib/test_lockup.ko` and `nmi_probe.ko` must match the exact test kernel;
  the variant runner rebuilds them per build directory.

## Existing test coverage found in the wild

- kvm-unit-tests (current GitLab tree): no arm64 NMI/pseudo-NMI tests; the
  old git.kernel.org repository only contains a relocation notice.
- Linux selftests: none for arm64 NMI.
- The series cover letter says testing was done on QEMU, Fast Model and
  pre-silicon and explicitly asks for real-hardware testing; no test module
  ships with the series.
- Reusable in-tree pieces used here: `lib/test_lockup.c`, sysrq-l,
  kprobes, perf events.

## Not covered

- KVM guest NMI, real SDEI event delivery, vendor SPI NMI users, and
  firmware handling of `HCRX_EL2.TALLINT` (documented boot requirement).
- GICv5 SPI/LPI NMIs other than the IPI/LPI domain path exercised by the
  GICv5 cases; GICv3 SPI NMI needs an in-kernel NMI requester that the tree
  does not have.
- `nmi_extra=suspend` depends on a working RTC wakeup source; on hardware
  verify the board's RTC wakeup before relying on it.
