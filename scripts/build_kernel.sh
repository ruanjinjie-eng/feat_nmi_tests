#!/bin/bash
# Build the arm64 kernel under test (out-of-tree build directory).
#
# Usage: build_kernel.sh [kernel-src]
#   kernel-src defaults to $KSRC or $SUITE_DIR/../linux_old1
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
KSRC="${1:-${KSRC:-$SUITE_DIR/../linux_old1}}"
BUILD="${BUILD_DIR:-$SUITE_DIR/build/linux}"
CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"
KDEFCONFIG="${KDEFCONFIG:-defconfig}"
FRAGMENT="$SUITE_DIR/configs/nmi.config"

echo "== kernel source : $KSRC"
echo "== build dir     : $BUILD"
echo "== cross prefix  : $CROSS"
echo "== jobs          : $JOBS"
git -C "$KSRC" log -1 --oneline

mkdir -p "$BUILD"

make -C "$KSRC" O="$BUILD" ARCH=arm64 CROSS_COMPILE="$CROSS" "$KDEFCONFIG"

# Merge the NMI test requirements into the platform configuration.  On real
# hardware set KDEFCONFIG=<vendor>_defconfig so the UART and storage drivers
# the board needs are present.
"$KSRC/scripts/kconfig/merge_config.sh" -m -O "$BUILD" \
	"$BUILD/.config" "$FRAGMENT" >/dev/null

# Options needed by the test suite (also documented in the fragment).
"$KSRC/scripts/config" --file "$BUILD/.config" \
	-e ARM64_NMI \
	-e ARM64_PSEUDO_NMI \
	-e DEBUG_FS \
	-e GENERIC_IRQ_DEBUGFS \
	-e MODULES \
	-e MAGIC_SYSRQ \
	-e IKCONFIG \
	-e IKCONFIG_PROC \
	-e SOFTLOCKUP_DETECTOR \
	-e HARDLOCKUP_DETECTOR \
	-m TEST_LOCKUP \
	-e DEBUG_IRQFLAGS \
	-e KPROBES \
	-e PERF_EVENTS \
	-e ARM_PMU \
	-e RTC_DRV_PL031 \
	-d MODULE_SIG \
	-d MODULE_SIG_FORCE \
	-d DEBUG_INFO \
	-e DEBUG_INFO_NONE \
	-d WERROR

# Extra options for config-variant builds, e.g. EXTRA_CONFIG="-d ARM64_NMI".
if [ -n "${EXTRA_CONFIG:-}" ]; then
	# shellcheck disable=SC2086
	"$KSRC/scripts/config" --file "$BUILD/.config" $EXTRA_CONFIG
fi

make -C "$KSRC" O="$BUILD" ARCH=arm64 CROSS_COMPILE="$CROSS" olddefconfig

echo "== resulting .config checks =="
for opt in CONFIG_ARM64_NMI CONFIG_ARM64_PSEUDO_NMI CONFIG_ARM_GIC \
	   CONFIG_ARM_GIC_V3 CONFIG_ARM_GIC_V5 CONFIG_GENERIC_IRQ_DEBUGFS \
	   CONFIG_MODULES CONFIG_MAGIC_SYSRQ CONFIG_SMP CONFIG_PRINTK_TIME \
	   CONFIG_SOFTLOCKUP_DETECTOR CONFIG_HARDLOCKUP_DETECTOR \
	   CONFIG_TEST_LOCKUP CONFIG_PERF_EVENTS CONFIG_ARM_PMU; do
	grep -E "^${opt}=" "$BUILD/.config" || echo "!! $opt not set"
done

make -C "$KSRC" O="$BUILD" ARCH=arm64 CROSS_COMPILE="$CROSS" -j"$JOBS" Image modules

ls -l "$BUILD/arch/arm64/boot/Image"
echo "== kernel build OK =="
