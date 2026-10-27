#!/bin/bash
# Panic-path test: leave a CPU spinning with IRQs disabled, then panic.
# smp_send_stop() cannot reach it with the normal stop IPI, so with NMI
# support it must fall back to IPI_CPU_STOP_NMI ("SMP: retry stop with NMI").
# Without NMI the CPU cannot be stopped and the kernel logs
# "SMP: failed to stop secondary CPUs".
#
# These cases end in a panic, so they are run separately from the normal
# matrix.  Logs are saved under logs/crash-<timestamp>/.
set -u

SUITE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="${QEMU:-$SUITE_DIR/../qemu/build/qemu-system-aarch64}"
IMAGE="$SUITE_DIR/build/linux/arch/arm64/boot/Image"
INITRD="$SUITE_DIR/build/initramfs.cpio.gz"
LOGDIR="$SUITE_DIR/logs/crash-$(date +%Y%m%d-%H%M%S)"
TIMEOUT="${TIMEOUT:-150}"

mkdir -p "$LOGDIR"
[ -x "$QEMU" ] || { echo "ERROR: QEMU not found: $QEMU"; exit 1; }
[ -f "$IMAGE" ] || { echo "ERROR: missing $IMAGE"; exit 1; }
[ -f "$INITRD" ] || { echo "ERROR: missing $INITRD"; exit 1; }

# name|cpu|gic|extra kernel args|expect NMI retry|expect failed-stop
CASES=(
"01_arch_crash|max|3||1|0"
"02_pseudo_crash|neoverse-n1|3|irqchip.gicv3_pseudo_nmi=1|1|0"
"03_no_nmi_crash|neoverse-n1|3||0|1"
"04_gicv5_crash|max,has_gcie=on|x-5||1|0"
)

pass=0
fail=0
declare -a failed_cases=()

for entry in "${CASES[@]}"; do
	IFS='|' read -r name cpu gic extra want_retry want_fail <<<"$entry"
	log="$LOGDIR/$name.log"

	echo "----------------------------------------------------------------"
	echo "== CRASH CASE $name (cpu=$cpu gic=$gic) extra='$extra'"

	# shellcheck disable=SC2086
	timeout "$TIMEOUT" "$QEMU" \
		-M "virt,gic-version=$gic" \
		-cpu "$cpu" -smp 2 -m 1024 \
		-kernel "$IMAGE" -initrd "$INITRD" \
		-append "console=ttyAMA0,115200 earlycon log_buf_len=2M panic=-1 nmi_expect=auto nmi_extra=crash $extra" \
		-nographic -no-reboot >"$log" 2>&1
	rc=$?

	reason=""
	grep -q "NMI_TEST: crash_begin=1" "$log" || reason="crash_begin missing"
	grep -q "Kernel panic" "$log" || reason="$reason; no panic"

	retry=$(grep -c "SMP: retry stop with NMI for CPUs" "$log")
	stop_failed=$(grep -c "SMP: failed to stop secondary CPUs" "$log")

	if [ "$retry" -ne "$want_retry" ]; then
		reason="$reason; expected retry=$want_retry got $retry"
	fi
	if [ "$want_fail" -eq 1 ] && [ "$stop_failed" -eq 0 ]; then
		reason="$reason; expected failed-stop message"
	fi
	if [ "$want_fail" -eq 0 ] && [ "$stop_failed" -ne 0 ]; then
		reason="$reason; CPUs could not be stopped"
	fi
	if grep -qE "WARNING: CPU:|kernel BUG at|Oops:" "$log"; then
		reason="$reason; unexpected WARNING/BUG"
	fi

	if [ -z "$reason" ]; then
		echo "   PASS (retry=$retry stop_failed=$stop_failed qemu_rc=$rc)"
		pass=$((pass + 1))
	else
		echo "   FAIL ($reason; retry=$retry stop_failed=$stop_failed qemu_rc=$rc)"
		fail=$((fail + 1))
		failed_cases+=("$name")
	fi
done

echo "================================================================"
echo "SUMMARY: pass=$pass fail=$fail"
[ "$fail" -ne 0 ] && printf 'FAILED: %s\n' "${failed_cases[*]}"
echo "logs: $LOGDIR"
exit "$fail"
