#!/usr/bin/env bash
#
# install.sh - one-shot builder for Lindroid on Xiaomi POCO X5 5G / Redmi Note 12 5G
#
# Takes a bare Debian/Ubuntu machine to a flashable pmOS zip that boots this
# phone with a working display, touch and a permanent root shell over USB.
# Every step is idempotent: what is already done is skipped, so the script can
# be re-run after a failure without starting over.
#
# Pipeline
# --------
#   1. verify host tools
#   2. fetch the kernel tree (or reuse it)
#   3. apply our patches
#   4. build the kernel           -> Image
#   5. build the boot image       -> boot.img, with the pmOS initramfs patched
#   6. run pmbootstrap init+install, or reuse the existing work dir
#   7. patch the recovery zip     -> A/B fixes, boot.img, getty, root password
#   8. verify the result
#
# Usage
# -----
#   ./install.sh                        # console UI (known good)
#   ./install.sh --ui weston            # pmOS with weston
#   ./install.sh --ui surfbgtk          # pmOS with the GTK shell
#   ./install.sh --skip-kernel          # reuse the Image already built
#   ./install.sh --skip-pmos            # stop after boot.img
#   ./install.sh --check                # report state, change nothing
#
# Options
# -------
#   --ui NAME            console | weston | surfbgtk        (default: console)
#   --device NAME        pmbootstrap device codename       (default: xiaomi-moonstone)
#   --kernel-dir PATH    kernel source tree     (default: ~/dm-kernel)
#   --work-dir PATH      pmbootstrap work dir   (default: ~/pmos-kernel)
#   --out-dir PATH       build artefacts        (default: ./out)
#   --boot IMG           use this boot.img instead of building one
#   --skip-kernel        do not build the kernel
#   --skip-pmos          do not build the recovery zip
#   --check              show what is present and exit
#
# Requirements
# ------------
#   sudo, clang (LLVM 21+), zip, unzip, python3, cpio, git, curl
#   pmbootstrap is installed automatically if missing.
#
# Flashing
# --------
#   adb sideload <zip>        from a TWRP or OrangeRecovery, or any AOSP
#                             recovery that accepts the update-binary pmOS ships
#   The phone is repartitioned during install. Stock Android is unaffected:
#   only the `boot` partition and the contents of `userdata` are touched.
#
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$HERE/tools"

KERNEL_DIR="$HOME/dm-kernel"
# Prefer whichever work dir already exists, so re-running on an existing
# machine does not silently start a second, empty one.
if [ -d "$HOME/pmos_kernel" ]; then
	WORK_DIR="$HOME/pmos_kernel"
else
	WORK_DIR="$HOME/pmos-kernel"
fi
OUT_DIR="$HERE/out"
AK3="$HOME/ak3-darkmoon"
DEVICE="xiaomi-moonstone"
UI="console"
CLANG_DIR="$HOME/Clang"

DO_KERNEL=1
DO_PMOS=1
CHECK_ONLY=0
BOOT_IMG=""

# --------------------------------------------------------------- output
if [ -t 1 ]; then
	B=$'\033[1m'; G=$'\033[32m'; Y=$'\033[33m'; R=$'\033[31m'; N=$'\033[0m'
else
	B=""; G=""; Y=""; R=""; N=""
fi
step() { printf '\n%s==> %s%s\n' "$B" "$*" "$N"; }
info() { printf '    %s\n' "$*"; }
ok()   { printf '    %s%s%s\n' "$G" "$*" "$N"; }
warn() { printf '    %s%s%s\n' "$Y" "$*" "$N"; }
die()  { printf '\n%serror: %s%s\n' "$R" "$*" "$N" >&2; exit 1; }

usage() { sed -n '/^# Usage/,/^# Flashing/p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

while [ $# -gt 0 ]; do
	case "$1" in
		--ui)         UI="${2:?--ui needs a value}"; shift 2 ;;
		--device)     DEVICE="${2:?--device needs a value}"; shift 2 ;;
		--kernel-dir) KERNEL_DIR="${2:?}"; shift 2 ;;
		--work-dir)   WORK_DIR="${2:?}"; shift 2 ;;
		--out-dir)    OUT_DIR="${2:?}"; shift 2 ;;
		--boot)       BOOT_IMG="${2:?--boot needs a path}"; shift 2 ;;
		--skip-kernel) DO_KERNEL=0; shift ;;
		--skip-pmos)   DO_PMOS=0; shift ;;
		--check)      CHECK_ONLY=1; shift ;;
		-h|--help)    usage ;;
		*)            die "unknown argument: $1 (try --help)" ;;
	esac
done

case "$UI" in
	console|weston|surfbgtk) ;;
	*) die "unknown UI '$UI'. Choose console, weston or surfbgtk." ;;
esac

# --------------------------------------------------------------- 1. tools
step "Checking host tools"

MISSING=""
for t in git curl zip unzip python3 cpio sudo; do
	command -v "$t" >/dev/null 2>&1 || MISSING="$MISSING $t"
done
[ -z "$MISSING" ] || die "missing host tools:$MISSING

On Debian/Ubuntu:
    sudo apt install git curl zip unzip python3 cpio sudo lz4"

if [ ! -x "$CLANG_DIR/bin/clang" ]; then
	warn "no LLVM toolchain at $CLANG_DIR"
	info  "the build expects clang 21 or newer there; install it and re-run"
else
	ok "clang $("$CLANG_DIR/bin/clang" --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
fi

if command -v pmbootstrap >/dev/null 2>&1; then
	ok "pmbootstrap $(pmbootstrap --version 2>&1 | head -1)"
else
	warn "pmbootstrap is missing, it will be installed"
	info  "install:  curl -s https://gitlab.alpinelinux.org/alpine/aports/-/raw/3.22-stable/community/pmbootstrap/APKBUILD"
	info  "or grab a release binary from https://gitlab.postmarketos.org/pmbootstrap"
fi

# --------------------------------------------------------------- state
report_state() {
	local img="$KERNEL_DIR/out/arch/arm64/boot/Image"
	printf '    kernel tree   %s\n' "$([ -d "$KERNEL_DIR" ] && echo present || echo missing)"
	printf '    kernel Image  %s\n' "$([ -f "$img" ] && echo "$(stat -c%s "$img") bytes, $(date -r "$img" '+%Y-%m-%d %H:%M')" || echo not built)"
	printf '    work dir      %s\n' "$([ -d "$WORK_DIR" ] && echo present || echo missing)"
	if [ -d "$WORK_DIR" ]; then
		local st
		st=$(cd "$WORK_DIR" && pmbootstrap status 2>&1 | tr -d '\033' | sed 's/\[[0-9;]*m//g')
		printf '      device      %s\n' "$(printf '%s' "$st" | sed -n 's/.*Device: *\([a-z0-9-]*\).*/\1/p')"
		printf '      ui          %s\n' "$(printf '%s' "$st" | sed -n 's/.*UI: *\([a-z]*\).*/\1/p')"
	fi
	printf '    out dir       %s\n' "$OUT_DIR"
	ls -1 "$OUT_DIR"/*.zip 2>/dev/null | sed 's/^/      /' || true
}

if [ "$CHECK_ONLY" = 1 ]; then
	step "Current state"
	report_state
	exit 0
fi

mkdir -p "$OUT_DIR"

# --------------------------------------------------------------- 2. kernel tree
step "Kernel source"
if [ ! -d "$KERNEL_DIR" ]; then
	die "no kernel tree at $KERNEL_DIR

Clone it first:
    git clone --depth 1 -b 16 https://github.com/kamikaonashi/kernel_xiaomi_stone.git $KERNEL_DIR
    cd $KERNEL_DIR && git apply $TOOLS/../patches/*.patch

Note: this must be the Darkmoon tree, not kernel_xiaomi_stone. The two differ
in CONFIG_LOCALVERSION ('-Darkmoon-Reborn' vs '-Protium') and only the first
one is what this phone runs."
fi

# The one check worth more than its own cost. Two trees live in this home
# directory and they look alike; the version string is the only reliable tell.
if ! grep -q 'LOCALVERSION="-Darkmoon-Reborn"' \
	"$KERNEL_DIR/arch/arm64/configs/stone_defconfig" 2>/dev/null; then
	die "$KERNEL_DIR is not the Darkmoon tree

It builds a different kernel. Look for CONFIG_LOCALVERSION=\"-Darkmoon-Reborn\"
in arch/arm64/configs/stone_defconfig."
fi
ok "Darkmoon tree confirmed"

if [ -d "$HERE/patches" ]; then
	# git apply --check отвергает и уже применённые патчи, поэтому одного его
	# мало. Признак "уже применено" - сам артефакт патча: без него в дереве
	# нет drivers/lindroid-drm, то есть первую половину патчей никто не
	# накладывал.
	if ( cd "$KERNEL_DIR" && git apply --check "$HERE"/patches/*.patch ) 2>/dev/null; then
		( cd "$KERNEL_DIR" && git apply "$HERE"/patches/*.patch )
		ok "patches applied"
	elif [ -d "$KERNEL_DIR/drivers/lindroid-drm" ]; then
		ok "patches already applied (drivers/lindroid-drm is in the tree)"
	else
		die "the patches do not apply and drivers/lindroid-drm is missing.

The tree is in a state this script does not know how to handle. Resolve it
by hand:

    cd $KERNEL_DIR
    git status
    git apply --check $HERE/patches/*.patch    # see why it refuses
    git apply     $HERE/patches/*.patch"
	fi
fi

# --------------------------------------------------------------- 3. kernel
if [ "$DO_KERNEL" = 1 ]; then
	step "Building the kernel (this takes about 20 minutes)"
	( cd "$KERNEL_DIR" && PATH="$CLANG_DIR/bin:$PATH" bash build.sh ) 2>&1 |
		grep -vE '^  (CC|LD|AR|AS|GEN|CHK|UPD|HOSTCC|WRAP|OBJCOPY|SYMLINK|CALL|DESCEND|INSTALL|SHIPPED|EXPORT|LEX|ZOFF|SYNSYM)\b' |
		tail -12
else
	step "Skipping the kernel build"
fi

IMG="$KERNEL_DIR/out/arch/arm64/boot/Image"
[ -f "$IMG" ] || die "no Image at $IMG. Build the kernel first (drop --skip-kernel)."

VER=$(strings -n 40 "$IMG" | grep -m1 -o 'Linux version [^ ]*' || true)
case "$VER" in
	*Darkmoon-Reborn*) ok "$VER" ;;
	*) die "built image reports '$VER', expected Darkmoon-Reborn" ;;
esac

# --------------------------------------------------------------- 4. boot.img
if [ -z "$BOOT_IMG" ]; then
	step "Building boot.img (pmOS initramfs is patched here)"
	BOOT_IMG="$OUT_DIR/boot-$UI.img"
	# LIND_CMDLINE and LIND_DEBUG_SHELL are left at their defaults: an empty
	# cmdline, which is what the stock header carries, and a normal boot into
	# stage 2. See tools/pmos-pmos-initramfs.sh for the switches.
	bash "$TOOLS/pmos-pmos-initramfs.sh" "$BOOT_IMG"
fi
[ -f "$BOOT_IMG" ] || die "no boot image at $BOOT_IMG"
BOOT_MD5=$(md5sum "$BOOT_IMG" | cut -d' ' -f1)
ok "$(basename "$BOOT_IMG")  $BOOT_MD5  $(stat -c%s "$BOOT_IMG") bytes"

[ "$DO_PMOS" = 1 ] || { step "Done: $BOOT_IMG"; exit 0; }

# --------------------------------------------------------------- 5. pmbootstrap
step "pmbootstrap"

if ! command -v pmbootstrap >/dev/null 2>&1; then
	die "pmbootstrap is required and is not installed. See the message above."
fi

# pmbootstrap shells out to sudo for every chroot step. Without a TTY the
# password prompt has nowhere to go and the run dies halfway through, and sudo
# binds its ticket to the terminal, so a sudo warmed up elsewhere does not help.
if ! sudo -n true 2>/dev/null; then
	[ -t 0 ] || die "sudo is required but this process has no TTY.

sudo ties its credential cache to the terminal, so a 'sudo -v' you ran in your
own shell does not reach this build. Run the builder yourself:

    cd $HERE && ./install.sh --ui $UI

If it still asks, run 'sudo -v' first in the same terminal."
	die "run 'sudo -v' first, then re-run this script in the same terminal"
fi
ok "sudo available without a prompt"

# The UI is a property of the work dir, set by 'init', not an argument to
# 'install'. Silently building a console zip because that is what happens to
# be configured is how you end up debugging the wrong thing.
NEED_INIT=0
if [ ! -d "$WORK_DIR" ] || [ ! -d "$WORK_DIR/chroot_rootfs_$DEVICE" ]; then
	NEED_INIT=1
	info "no work dir, running pmbootstrap init"
else
	HAVE_UI=$(cd "$WORK_DIR" && pmbootstrap status 2>&1 | tr -d '\033' |
		sed 's/\[[0-9;]*m//g' | sed -n 's/.*UI: *\([a-z]*\).*/\1/p')
	if [ "$HAVE_UI" != "$UI" ]; then
		die "the work dir is configured for UI '$HAVE_UI', you asked for '$UI'.

The UI is baked into the work directory. Changing it means running
'pmbootstrap init' again, which touches the chroots and downloads packages.
Use a separate work dir to keep both, for example:

    ./install.sh --ui $UI --work-dir $WORK_DIR-$UI"
	fi
	ok "work dir ready (device $DEVICE, ui $UI)"
fi

if [ "$NEED_INIT" = 1 ]; then
	# --force is required on a fresh dir, and is what makes init wipe
	# existing state. Only ever used where there is nothing to lose.
	( cd "$HOME" && PATH="$CLANG_DIR/bin:$PATH" pmbootstrap init \
		--work-dir "$WORK_DIR" \
		--device "$DEVICE" \
		--ui "$UI" \
		--force ) 2>&1 | tail -8
fi

step "Installing pmOS and building the recovery zip"
# --password is a dummy: it only silences the interactive "Choose a password
# for the user ..." prompt. The root password in the finished system is
# cleared further down by pmos-patch-rootfs.py.
( cd "$WORK_DIR" && pmbootstrap install --android-recovery-zip --password lindroid ) 2>&1 |
	tail -5

# pmbootstrap 3.11 drops the zip inside the buildroot chroot, not in images/.
PMOS_ZIP=$(ls -t "$WORK_DIR"/chroot_buildroot_aarch64/var/lib/postmarketos-android-recovery-installer/*.zip 2>/dev/null | head -1 || true)
[ -n "$PMOS_ZIP" ] || PMOS_ZIP=$(ls -t "$WORK_DIR"/images/*pmos*.zip 2>/dev/null | head -1 || true)
[ -n "$PMOS_ZIP" ] || die "pmbootstrap finished but produced no zip"
ok "$(basename "$PMOS_ZIP")  $(stat -c%s "$PMOS_ZIP") bytes"

# --------------------------------------------------------------- 6. patches
step "Patching the recovery zip"

AB_ZIP="$OUT_DIR/pmos-$UI-ab.zip"
info "pointing the installer at userdata"
# pmos-patch-ab.sh rewrites the zip in place and takes only the path plus the
# slot, so it gets a copy. The one pmbootstrap produced stays pristine, which
# matters because re-running the build would otherwise compound the edits.
cp -p "$PMOS_ZIP" "$AB_ZIP"
bash "$TOOLS/pmos-patch-ab.sh" "$AB_ZIP" _a

FINAL_ZIP="$OUT_DIR/pmos-$UI-$DEVICE-$(date -u +%Y%m%d).zip"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
info "injecting boot.img, USB getty and an unlocked root"
# Three edits inside rootfs.tar.gz. Without the last two a fresh install boots
# and is then unreachable: pmOS' own USB setup brings up NCM but never ACM, so
# no /dev/ttyGS0 appears and nothing listens on the USB serial port; and root
# ships as '!' in /etc/shadow, which is a locked account, not a password.
python3 "$TOOLS/pmos-patch-rootfs.py" "$AB_ZIP" "$FINAL_ZIP" \
	--boot "$BOOT_IMG" --unlock-root --usb-getty ttyGS0 --work "$WORK"

# --------------------------------------------------------------- 7. verify
step "Verifying"

unzip -l "$FINAL_ZIP" | grep -qE 'update-binary' || die "no update-binary in the zip"
ok "update-binary present"

unzip -l "$FINAL_ZIP" | grep -qE ' rootfs\.tar\.gz$' || die "no rootfs.tar.gz in the zip"
ok "rootfs.tar.gz present"

INNER=$(unzip -p "$FINAL_ZIP" rootfs.tar.gz | tar -xOzf - ./boot/boot.img 2>/dev/null |
	md5sum | cut -d' ' -f1)
[ "$INNER" = "$BOOT_MD5" ] || die "boot.img inside the archive is $INNER, expected $BOOT_MD5"
ok "boot.img matches the one we built"

unzip -p "$FINAL_ZIP" rootfs.tar.gz | tar -xOzf - ./etc/inittab 2>/dev/null |
	grep -q ttyGS0 || die "no ttyGS0 getty in the image"
ok "USB getty present"

unzip -p "$FINAL_ZIP" rootfs.tar.gz | tar -xOzf - ./etc/shadow 2>/dev/null |
	grep -q '^root::' || warn "root is still locked in /etc/shadow"

unzip -p "$AB_ZIP" chroot/install_options 2>/dev/null |
	grep -q "INSTALL_PARTITION='userdata'" || die "install target is not userdata"
ok "install target is userdata"

FINAL_MD5=$(md5sum "$FINAL_ZIP" | cut -d' ' -f1)

step "Done"
printf '    zip:   %s\n' "$FINAL_ZIP"
printf '    size:  %s bytes\n' "$(stat -c%s "$FINAL_ZIP")"
printf '    md5:   %s\n\n' "$FINAL_MD5"
cat <<EOF
    Flash it with:
        adb sideload $FINAL_ZIP

    from a TWRP or OrangeRecovery. It repartitions userdata during install,
    so everything on it is lost. Stock Android is untouched and can be
    restored with:

        fastboot flash boot ~/kernel_xiaomi_stone/stock/boot.img

    First boot takes about a minute. When it settles you get a login prompt
    on the phone screen and, over the same USB cable, on /dev/ttyACM0:

        login: root      (no password)

EOF
