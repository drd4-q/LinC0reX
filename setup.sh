#!/usr/bin/env bash
#
# Prepare a Darkmoon kernel tree for LinC0reX.
#
# The display-control driver this installs is Lindroid's, carried over as
# drivers/lindroid-drm/; we did not write it. Clones the base tree if needed,
# wires in KernelSU-Next, applies our patches and drops the driver in.
# Idempotent: re-running is a no-op once set up.
#
#   ./setup.sh [target-dir]     default: ~/dm-kernel
#
# Afterwards build with ./build.sh in the target directory.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="${1:-$HOME/dm-kernel}"

KERNEL_REPO="https://github.com/kamikaonashi/kernel_xiaomi_stone.git"
KERNEL_BRANCH="16"
KSU_SETUP="https://raw.githubusercontent.com/KernelSU-Next/KernelSU-Next/next/kernel/setup.sh"

say() { printf '\n=== %s ===\n' "$*"; }

say "Cloning base tree -> $TARGET"
if [ -d "$TARGET/.git" ]; then
    echo "already cloned, skipping"
else
    git clone -b "$KERNEL_BRANCH" --depth=1 "$KERNEL_REPO" "$TARGET"
fi

cd "$TARGET"

say "KernelSU-Next (legacy branch, required for 5.4)"
if [ -L drivers/kernelsu ] || [ -d KernelSU-Next ]; then
    echo "already integrated, skipping"
else
    # The 'legacy' branch is required: the current one only supports 5.10+.
    curl -LSsf "$KSU_SETUP" | bash -s legacy
fi

say "Applying patches"
for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    if git apply --reverse --check "$p" 2>/dev/null; then
        echo "already applied: $(basename "$p")"
    else
        git apply "$p"
        echo "applied: $(basename "$p")"
    fi
done

say "Installing the lindroid-drm driver (Lindroid's, not ours)"
mkdir -p drivers/lindroid-drm
cp -r "$HERE/drivers/lindroid-drm/." drivers/lindroid-drm/
echo "driver copied"

say "Installing build scripts"
cp -f "$HERE/build.sh" .
cp -f "$HERE/ak3-darkmoon-anykernel.sh" .
chmod +x build.sh
echo "build.sh, ak3-darkmoon-anykernel.sh"

cat <<'EOF'

Done. Build with:

    cd <target> && ./build.sh LinC0reX

The flashable zip lands next to build.sh.

Note: the driver does NOT create a DRM card at boot
(EVDI_DEFAULT_NR_DEVICES=0). That is deliberate — creating it early takes
/dev/dri/card0 away from the real display driver and Android's SurfaceFlinger
stops starting, leaving the phone on the boot logo. The container-side
create-disp creates the card on demand instead:

    echo 1 > /sys/devices/evdi-lindroid/add
EOF
