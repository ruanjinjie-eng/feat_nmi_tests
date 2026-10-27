#!/bin/bash
# Build and boot the Kconfig variants that the NMI series introduces:
#   - CONFIG_ARM64_NMI=n       (architected NMI compiled out)
#   - CONFIG_ARM64_PSEUDO_NMI=n
#
# Each variant gets its own kernel build directory, modules and initramfs.
# The guest runs in nmi_expect=auto mode, so the same checks apply.
set -u

SUITE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="${QEMU:-$SUITE_DIR/../qemu/build/qemu-system-aarch64}"
KSRC="${KSRC:-$SUITE_DIR/../linux_old1}"
LOGDIR="$SUITE_DIR/logs/config-$(date +%Y%m%d-%H%M%S)"
TIMEOUT="${TIMEOUT:-150}"
DO_BUILD="${DO_BUILD:-1}"

mkdir -p "$LOGDIR"

build_variant()
{
	local name="$1" extra="$2"
	local bdir="$SUITE_DIR/build/linux-$name"
	local initrd="$SUITE_DIR/build/initramfs-$name.cpio.gz"

	echo "==== building variant $name ($extra)"
	BUILD_DIR="$bdir" EXTRA_CONFIG="$extra" \
		"$SUITE_DIR/scripts/build_kernel.sh" || return 1
	BUILD_DIR="$bdir" OUT_INITRD="$initrd" KSRC="$KSRC" \
		"$SUITE_DIR/scripts/build_guest.sh" || return 1
}

run_variant()
{
	local name="$1" bdir="$2" initrd="$3" label="$4"
	local cpu="$5" gic="$6" kextra="$7"
	local log="$LOGDIR/${name}_${label}.log"

	echo "----------------------------------------------------------------"
	echo "== $name / $label (cpu=$cpu gic=$gic) kernel='$kextra'"
	# shellcheck disable=SC2086
	timeout "$TIMEOUT" "$QEMU" \
		-M "virt,gic-version=$gic" \
		-cpu "$cpu" -smp 2 -m 1024 \
		-kernel "$bdir/arch/arm64/boot/Image" -initrd "$initrd" \
		-append "console=ttyAMA0,115200 earlycon log_buf_len=2M panic=-1 nmi_expect=auto nmi_extra=perf nmi_runs=1 $kextra" \
		-nographic -no-reboot >"$log" 2>&1

	if grep -q "NMI_TEST: RESULT PASS" "$log" &&
	   ! grep -qE "WARNING: CPU:|kernel BUG at|Oops:" "$log"; then
		grep -m1 "NMI_TEST: auto_mode=" "$log" | sed 's/^/   /'
		echo "   PASS"
		return 0
	else
		echo "   FAIL ($log)"
		grep -m3 -E "NMI_TEST: FAIL|Kernel panic|WARNING: CPU:" "$log" |
			sed 's/^/   /'
		return 1
	fi
}

pass=0
fail=0

if [ "$DO_BUILD" -eq 1 ]; then
	build_variant nonmi "-d ARM64_NMI"   || { echo "build failed: nonmi"; exit 1; }
	build_variant nopseudo "-d ARM64_PSEUDO_NMI" || { echo "build failed: nopseudo"; exit 1; }
fi

NONMI="$SUITE_DIR/build/linux-nonmi"
NONMI_INITRD="$SUITE_DIR/build/initramfs-nonmi.cpio.gz"
NOPSEUDO="$SUITE_DIR/build/linux-nopseudo"
NOPSEUDO_INITRD="$SUITE_DIR/build/initramfs-nopseudo.cpio.gz"

run_variant nonmi "$NONMI" "$NONMI_INITRD" arch_nmi_compiled_out \
	max 3 "" && pass=$((pass + 1)) || fail=$((fail + 1))
run_variant nonmi "$NONMI" "$NONMI_INITRD" pseudo_still_available \
	max 3 "irqchip.gicv3_pseudo_nmi=1" && pass=$((pass + 1)) || fail=$((fail + 1))
run_variant nonmi "$NONMI" "$NONMI_INITRD" no_features \
	neoverse-n1 3 "" && pass=$((pass + 1)) || fail=$((fail + 1))

run_variant nopseudo "$NOPSEUDO" "$NOPSEUDO_INITRD" arch_nmi_unaffected \
	max 3 "irqchip.gicv3_pseudo_nmi=1" && pass=$((pass + 1)) || fail=$((fail + 1))
run_variant nopseudo "$NOPSEUDO" "$NOPSEUDO_INITRD" no_features \
	neoverse-n1 3 "irqchip.gicv3_pseudo_nmi=1" && pass=$((pass + 1)) || fail=$((fail + 1))

# The variant builds overwrite module/nmi_probe.ko; restore the base
# artifacts so the normal matrix keeps working.
echo "== restoring base build artifacts"
"$SUITE_DIR/scripts/build_guest.sh" >/dev/null 2>&1 || true

echo "================================================================"
echo "CONFIG VARIANTS SUMMARY: pass=$pass fail=$fail"
echo "logs: $LOGDIR"
exit "$fail"
