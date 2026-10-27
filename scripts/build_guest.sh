#!/bin/bash
# Build the probe module, the static guest init and the initramfs.
set -euo pipefail

SUITE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
KSRC="${KSRC:-$SUITE_DIR/../linux_old1}"
BUILD="${BUILD_DIR:-$SUITE_DIR/build/linux}"
OUT_INITRD="${OUT_INITRD:-$SUITE_DIR/build/initramfs.cpio.gz}"
CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"

[ -f "$BUILD/.config" ] || { echo "kernel not configured yet: $BUILD/.config"; exit 1; }

make -C "$KSRC" O="$BUILD" ARCH=arm64 CROSS_COMPILE="$CROSS" \
	M="$SUITE_DIR/module" -j"$JOBS" modules

make -C "$SUITE_DIR/guest" CC="${CROSS}gcc"

rm -rf "$SUITE_DIR/build/initramfs"
mkdir -p "$SUITE_DIR/build/initramfs/dev"
cp "$SUITE_DIR/guest/init" "$SUITE_DIR/build/initramfs/init"
cp "$SUITE_DIR/module/nmi_probe.ko" "$SUITE_DIR/build/initramfs/"

# Upstream lockup detector test module (hardlockup / perf NMI watchdog).
if [ -f "$BUILD/lib/test_lockup.ko" ]; then
	cp "$BUILD/lib/test_lockup.ko" "$SUITE_DIR/build/initramfs/"
else
	echo "!! $BUILD/lib/test_lockup.ko missing (CONFIG_TEST_LOCKUP=m)"
fi
chmod +x "$SUITE_DIR/build/initramfs/init"
mknod "$SUITE_DIR/build/initramfs/dev/console" c 5 1

(cd "$SUITE_DIR/build/initramfs" && find . | cpio -o -H newc --quiet) | \
	gzip -9 > "$OUT_INITRD"

echo "== initramfs =="
zcat "$OUT_INITRD" | cpio -t --quiet
ls -l "$OUT_INITRD" "$SUITE_DIR/module/nmi_probe.ko"
