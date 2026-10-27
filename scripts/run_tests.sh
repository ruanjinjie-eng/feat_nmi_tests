#!/bin/bash
# Run the FEAT_NMI QEMU test matrix.
#
# Each case boots the kernel under test with a specific CPU/GIC/command-line
# combination.  The guest init compares the observed state against the
# expectation passed on the command line and prints
#   NMI_TEST: RESULT PASS|FAIL
# The full serial log is saved under logs/<timestamp>/.
set -u

SUITE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="${QEMU:-$SUITE_DIR/../qemu/build/qemu-system-aarch64}"
QEMU_EFI="${QEMU_EFI:-/usr/share/qemu-efi-aarch64/QEMU_EFI.fd}"
KSRC="${KSRC:-$SUITE_DIR/../linux_old1}"
IMAGE="$SUITE_DIR/build/linux/arch/arm64/boot/Image"
INITRD="$SUITE_DIR/build/initramfs.cpio.gz"
LOGDIR="$SUITE_DIR/logs/$(date +%Y%m%d-%H%M%S)"
TIMEOUT="${TIMEOUT:-180}"

mkdir -p "$LOGDIR"

[ -x "$QEMU" ] || { echo "ERROR: QEMU not found: $QEMU"; exit 1; }
[ -f "$IMAGE" ] || { echo "ERROR: kernel Image not found: $IMAGE"; exit 1; }
[ -f "$INITRD" ] || { echo "ERROR: initramfs not found: $INITRD"; exit 1; }

echo "== QEMU: $("$QEMU" --version | head -1)"
echo "== kernel: $(git -C "$KSRC" log -1 --oneline 2>/dev/null || echo "$KSRC")"
echo "== logs: $LOGDIR"

if ! "$QEMU" -device arm-gicv3,help 2>&1 | grep -q has-nmi; then
	echo "!! WARNING: this QEMU has no GICv3 'has-nmi' property (FEAT_NMI support)"
fi

# name|cpu|gic-version|extra QEMU args|extra kernel args|expectations
CASES=(
"01_no_nmi|neoverse-n1|3|||supports:0,active:0,pseudo:0,gic_nmi:0,pseudo_log:0,arch_off_log:0,nmi_irqs:0,ipi_nmi:0,pmu_log:0,fast:0,bc_fast:0,bc_cpus:gt0"
"02_pseudo_nmi|neoverse-n1|3||irqchip.gicv3_pseudo_nmi=1|supports:0,active:0,pseudo:1,gic_nmi:0,pseudo_log:1,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0"
"03_arch_nmi|max|3|||supports:1,active:1,pseudo:0,gic_nmi:1,pseudo_log:0,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0"
"04_pe_nmi_gicv2|max|2|||supports:1,active:1,pseudo:0,gic_nmi:0,pseudo_log:0,arch_off_log:0,nmi_irqs:0,ipi_nmi:0,pmu_log:0,fast:0,bc_fast:0,bc_cpus:gt0"
"05_gic_nmi_no_pe|neoverse-n1|3|-global arm-gicv3.has-nmi=on||supports:0,active:0,pseudo:0,gic_nmi:1,pseudo_log:0,arch_off_log:0,nmi_irqs:0,ipi_nmi:0,pmu_log:0,fast:0,bc_fast:0,bc_cpus:gt0"
"06_arch_plus_pseudo|max|3||irqchip.gicv3_pseudo_nmi=1|supports:1,active:0,pseudo:1,gic_nmi:1,pseudo_log:1,arch_off_log:1,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0"
"07_idreg_nmi_off|max|3||id_aa64pfr1.nmi=0|supports:0,active:0,pseudo:0,gic_nmi:1,pseudo_log:0,arch_off_log:0,nmi_irqs:0,ipi_nmi:0,pmu_log:0,fast:0,bc_fast:0,bc_cpus:gt0"
"08_idreg_off_pseudo|max|3||id_aa64pfr1.nmi=0 irqchip.gicv3_pseudo_nmi=1|supports:0,active:0,pseudo:1,gic_nmi:1,pseudo_log:1,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0"
"09_gicv5_arch_nmi|max,has_gcie=on|x-5|||supports:1,active:1,pseudo:0,gic_nmi:any,pseudo_log:0,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0"
"10_gicv5_pseudo_arg|max,has_gcie=on|x-5||irqchip.gicv3_pseudo_nmi=1|supports:1,active:1,pseudo:0,gic_nmi:any,pseudo_log:0,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0"
"11_gicv4_arch_nmi|max|4,virtualization=on|||supports:1,active:1,pseudo:0,gic_nmi:1,pseudo_log:0,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0"
"12_pseudo_on_gicv2|neoverse-n1|2||irqchip.gicv3_pseudo_nmi=1|supports:0,active:0,pseudo:0,gic_nmi:0,pseudo_log:0,arch_off_log:0,nmi_irqs:0,ipi_nmi:0,pmu_log:0,fast:0,bc_fast:0,bc_cpus:gt0"
"13_arch_stress_hotplug|max|3||nmi_extra=sysrq,perf,stress,hotplug nmi_runs=3|supports:1,active:1,pseudo:0,gic_nmi:1,pseudo_log:0,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0,sysrq_bt:1,perf_ok:1,stress_ok:1,stress_pmu_delta:gt0,stress_bt:gt0,hotplug_ok:1,retest_fast:1"
"14_pseudo_hardlockup|neoverse-n1|3||irqchip.gicv3_pseudo_nmi=1 nmi_extra=perf,hardlockup|supports:0,active:0,pseudo:1,gic_nmi:0,pseudo_log:1,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0,perf_ok:1,hardlockup:1"
"15_arch_hardlockup|max|3||nmi_extra=perf,hardlockup|supports:1,active:1,pseudo:0,gic_nmi:1,pseudo_log:0,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0,perf_ok:1,hardlockup:1"
"16_no_nmi_stress|neoverse-n1|3||nmi_extra=perf,stress|supports:0,active:0,pseudo:0,gic_nmi:0,pseudo_log:0,arch_off_log:0,nmi_irqs:0,ipi_nmi:0,pmu_log:0,fast:0,bc_fast:0,bc_cpus:gt0,perf_ok:1,stress_ok:1,stress_pmu_delta:gt0,stress_bt:gt0"
"17_pseudo_stress|neoverse-n1|3||irqchip.gicv3_pseudo_nmi=1 nmi_extra=perf,stress|supports:0,active:0,pseudo:1,gic_nmi:0,pseudo_log:1,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0,perf_ok:1,stress_ok:1,stress_pmu_delta:gt0,stress_bt:gt0"
"18_arch_suspend|max|3||nmi_extra=suspend|supports:1,active:1,pseudo:0,gic_nmi:1,pseudo_log:0,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0,suspend_ok:1,retest_fast:1"
"19_uefi_acpi_arch|max|3|-bios @QEMU_EFI@|nmi_extra=perf,efi|supports:1,active:1,pseudo:0,gic_nmi:1,pseudo_log:0,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0,perf_ok:1,efi_ok:1"
"20_uefi_acpi_pseudo|neoverse-n1|3|-bios @QEMU_EFI@|irqchip.gicv3_pseudo_nmi=1 nmi_extra=perf,efi|supports:0,active:0,pseudo:1,gic_nmi:0,pseudo_log:1,arch_off_log:0,nmi_irqs:gt0,ipi_nmi:gt0,pmu_log:1,fast:1,bc_fast:1,bc_cpus:gt0,perf_ok:1,efi_ok:1"
)

pass=0
fail=0
skip=0
declare -a failed_cases=()

for entry in "${CASES[@]}"; do
	IFS='|' read -r name cpu gic qextra extra expect <<<"$entry"

	if [[ "$qextra" == *@QEMU_EFI@* ]] && [ ! -f "$QEMU_EFI" ]; then
		echo "----------------------------------------------------------------"
		echo "== CASE $name: SKIP (UEFI firmware not found: $QEMU_EFI)"
		skip=$((skip + 1))
		continue
	fi
	qextra="${qextra//@QEMU_EFI@/$QEMU_EFI}"

	log="$LOGDIR/$name.log"
	qargs=()
	if [ -n "$qextra" ]; then
		read -ra qargs <<<"$qextra"
	fi

	echo "----------------------------------------------------------------"
	echo "== CASE $name (cpu=$cpu gic=$gic) qemu='${qextra}' kernel='${extra}'"
	echo "   $log"

	timeout "$TIMEOUT" "$QEMU" "${qargs[@]}" \
		-M "virt,gic-version=$gic" \
		-cpu "$cpu" -smp 2 -m 1024 \
		-kernel "$IMAGE" -initrd "$INITRD" \
		-append "console=ttyAMA0,115200 earlycon log_buf_len=2M panic=-1 nmi_expect=$expect $extra" \
		-nographic -no-reboot >"$log" 2>&1
	rc=$?

	if grep -q "NMI_TEST: RESULT PASS" "$log"; then
		echo "   PASS"
		pass=$((pass + 1))
	else
		reason=$(grep -m3 -E "NMI_TEST: FAIL|Kernel panic|NMI_TEST: RESULT" "$log" | tr '\n' ' ')
		echo "   FAIL (qemu rc=$rc) ${reason}"
		fail=$((fail + 1))
		failed_cases+=("$name")
	fi
done

echo "================================================================"
echo "SUMMARY: pass=$pass fail=$fail skip=$skip"
if [ "$fail" -ne 0 ]; then
	printf 'FAILED: %s\n' "${failed_cases[*]}"
fi
echo "logs: $LOGDIR"

{
	echo "kernel: $(git -C "$KSRC" log -1 --oneline 2>/dev/null || echo "$KSRC")"
	echo "qemu:   $("$QEMU" --version | head -1)"
	echo "pass=$pass fail=$fail skip=$skip"
	for entry in "${CASES[@]}"; do
		IFS='|' read -r name cpu gic qextra extra expect <<<"$entry"
		if [ ! -f "$LOGDIR/$name.log" ]; then
			echo "SKIP $name"
		elif grep -q "NMI_TEST: RESULT PASS" "$LOGDIR/$name.log"; then
			echo "PASS $name"
		else
			echo "FAIL $name"
		fi
	done
} > "$LOGDIR/summary.txt"

exit "$fail"
