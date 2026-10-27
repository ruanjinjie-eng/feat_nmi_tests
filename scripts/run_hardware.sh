#!/bin/bash
# Run the FEAT_NMI test suite on real arm64 hardware.
#
# Modes:
#   ./run_hardware.sh --print      print kexec/GRUB/U-Boot/PXE recipes
#   ./run_hardware.sh --kexec      build and kexec into the test kernel
#   ./run_hardware.sh --build-only build the kernel + initramfs only
#
# Environment:
#   KSRC=<kernel tree>        default ../linux_old1
#   KDEFCONFIG=<defconfig>    platform defconfig, default "defconfig".
#                             For a board, use e.g. KDEFCONFIG=defconfig or
#                             your vendor defconfig so the UART/storage
#                             drivers are present.
#   CONSOLE=<console>         default "ttyAMA0,115200" (e.g. ttyS0,115200)
#   EXTRA=<tests>             default "perf,sysrq"; use "all" for
#                             hotplug,hardlockup,suspend,efi too.
#                             NOTE: hardlockup locks a CPU for ~14 s.
#   RUNS=<n>                  IPI/broadcast iterations, default 3
#   BC_MAX=<n>                max CPUs in the broadcast test, default 4
#   SHUTDOWN=halt|reboot|poweroff   default halt (safe on real hardware)
#   BOOTARGS_EXTRA=<args>     extra kernel command line arguments
#
# The guest prints NMI_TEST: lines to the console and then halts/reboots.
set -u

SUITE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
KSRC="${KSRC:-$SUITE_DIR/../linux_old1}"
BUILD="$SUITE_DIR/build/linux"
IMAGE="$BUILD/arch/arm64/boot/Image"
INITRD="$SUITE_DIR/build/initramfs.cpio.gz"
CONSOLE="${CONSOLE:-ttyAMA0,115200}"
EXTRA="${EXTRA:-perf,sysrq}"
RUNS="${RUNS:-3}"
BC_MAX="${BC_MAX:-4}"
SHUTDOWN="${SHUTDOWN:-halt}"
BOOTARGS_EXTRA="${BOOTARGS_EXTRA:-}"
KDEFCONFIG="${KDEFCONFIG:-defconfig}"

MODE="print"
DO_BUILD=1

for arg in "$@"; do
	case "$arg" in
	--print) MODE="print" ;;
	--kexec) MODE="kexec" ;;
	--build-only) MODE="build" ;;
	--no-build) DO_BUILD=0 ;;
	--help|-h)
		sed -n '2,40p' "$0"
		exit 0
		;;
	*)
		echo "unknown argument: $arg (try --help)"
		exit 1
		;;
	esac
done

CMDLINE="console=$CONSOLE earlycon log_buf_len=2M nmi_expect=auto"
CMDLINE="$CMDLINE nmi_extra=$EXTRA nmi_runs=$RUNS nmi_bc_max=$BC_MAX"
CMDLINE="$CMDLINE nmi_shutdown=$SHUTDOWN $BOOTARGS_EXTRA"

if [ "$DO_BUILD" -eq 1 ]; then
	echo "== building kernel ($KDEFCONFIG) and initramfs"
	KDEFCONFIG="$KDEFCONFIG" "$SUITE_DIR/scripts/build_kernel.sh"
	KSRC="$KSRC" "$SUITE_DIR/scripts/build_guest.sh"
fi

[ -f "$IMAGE" ] || { echo "ERROR: missing $IMAGE"; exit 1; }
[ -f "$INITRD" ] || { echo "ERROR: missing $INITRD"; exit 1; }

echo
echo "== kernel : $IMAGE"
echo "== initrd : $INITRD"
echo "== cmdline: $CMDLINE"
echo

if [ "$MODE" = "build" ]; then
	exit 0
fi

if [ "$MODE" = "kexec" ]; then
	if [ "$(uname -m)" != "aarch64" ]; then
		echo "ERROR: kexec mode must run on the arm64 target"
		exit 1
	fi
	command -v kexec >/dev/null ||
		{ echo "ERROR: kexec-tools not installed on the target"; exit 1; }
	echo "!! kexec will replace the running kernel."
	echo "!! The guest halts/reboots at the end: nmi_shutdown=$SHUTDOWN"
	sleep 3
	kexec -l "$IMAGE" --initrd="$INITRD" --command-line="$CMDLINE" &&
		kexec -e
	exit $?
fi

cat <<EOF
================ kexec (run on the target) ================
kexec -l $IMAGE --initrd=$INITRD \\
      --command-line="$CMDLINE"
kexec -e

================ GRUB ================
menuentry "feat_nmi_test" {
    linux /boot/Image $CMDLINE
    initrd /boot/initramfs.cpio.gz
}

================ U-Boot (TFTP) ================
setenv bootargs '$CMDLINE'
tftp \$kernel_addr_r $IMAGE
tftp \$ramdisk_addr_r $INITRD
booti \$kernel_addr_r \$ramdisk_addr_r:\$filesize \$fdt_addr_r

================ PXE ================
kernel Image $CMDLINE
initrd initramfs.cpio.gz

================ notes ================
EOF
cat <<'EOF'
- Boot the target with the Image + initramfs above.  The guest init mounts
  proc/sys/debugfs, loads /nmi_probe.ko, runs the tests and prints
  "NMI_TEST: ..." lines on the console.  nmi_expect=auto derives the
  expected results from the detected capabilities, so the same image
  works on all three platform flavours.

- nmi_extra=perf,sysrq is safe.  "all" adds hotplug (offline/online CPU1),
  suspend (RTC wakeup), efi (efivarfs read) and hardlockup (perf/NMI
  watchdog via lib/test_lockup.ko, locks a CPU for ~14 s with IRQs off,
  hardlockup_panic is forced to 0).

- External NMI sources cannot be requested from userspace.  The suite
  exercises the two generic in-kernel NMI users: the IPI backtrace path
  (also used by sysrq-l) and the arm_pmu perf IRQ (also used by the
  hardlockup watchdog).  Vendor SDEI/SPI NMI drivers would need their own
  test hooks.
EOF
