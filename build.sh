#!/usr/bin/env bash
#
# Build the Darkmoon-Reborn kernel for Poco X5 5G / Redmi Note 12 5G (sunstone).
#
# Base tree: https://github.com/kamikaonashi/kernel_xiaomi_stone @ branch 16
# This is the same tree the shipped stock kernel is built from
# (uname -r == 5.4.302-Darkmoon-Reborn), which is why it works with the ROM
# where the Protium fork does not.
#
# The upstream tree ships no build script, so this is one.
#
# Usage:
#   ./build.sh                 build + package
#   ./build.sh pack            package only (no kernel build)

set -e

PREFIX="$HOME"
CLANG_DIR="$PREFIX/Clang"
export PATH="$CLANG_DIR/bin:$PATH"

KERNEL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AK3="$HOME/ak3-darkmoon"
DEFCONFIG="stone_defconfig"
ZIP_NAME="${1:-Darkmoon}"
JOBS="$(nproc --all)"

mkdir -p "$KERNEL_DIR/out"

# Must match the stock kernel's identity so nothing keys off the string.
export KBUILD_BUILD_USER="${KBUILD_BUILD_USER:-kami}"
export KBUILD_BUILD_HOST="${KBUILD_BUILD_HOST:-yourMom}"
export KBUILD_BUILD_VERSION=1
export KBUILD_BUILD_TIMESTAMP="Mon Apr  6 17:56:23 CEST 2026"

ARGS=(
    ARCH=arm64
    SUBARCH=arm64
    O=out
    CC="clang"
    AR="llvm-ar"
    NM="llvm-nm"
    LD="ld.lld -S"
    OBJCOPY="llvm-objcopy"
    OBJDUMP="llvm-objdump"
    STRIP="llvm-strip"
    CLANG_TRIPLE="aarch64-linux-gnu-"
    CROSS_COMPILE="aarch64-linux-gnu-"
    CROSS_COMPILE_ARM32="arm-linux-gnueabi-"
    CROSS_COMPILE_COMPAT="arm-linux-gnueabi-"
    LLVM=1
    LLVM_IAS=1
    INSTALL_MOD_STRIP=1
    # The shipped kernel is built with clang 21; newer clang flags implicit
    # enum-to-enum conversions as an error, and the out-of-tree qcacld-3.0
    # WiFi driver has plenty of those.  Silence just that diagnostic.
    KCFLAGS="-Wno-implicit-enum-enum-cast"
)

build_kernel() {
    echo "=== [1/2] defconfig ($DEFCONFIG) ==="
    make "$DEFCONFIG" "${ARGS[@]}"

    echo "=== [2/2] Building kernel ==="
    make -j"$JOBS" "${ARGS[@]}"
}

package() {
    echo "=== Packaging ==="
    local outd="$KERNEL_DIR/out/arch/arm64/boot"

    [ -f "$outd/Image" ] || { echo "no Image produced"; exit 1; }

    rm -rf "$AK3"
    git clone --depth=1 -b stone \
        https://github.com/gawasvedraj/AnyKernel3.git "$AK3"
    rm -rf "$AK3/.git" "$AK3/.github" "$AK3/README.md" "$AK3/dtb" "$AK3/dtbo"
    # anykernel.sh lives next to this script, outside the cloned template.
    cp -f "$KERNEL_DIR/ak3-darkmoon-anykernel.sh" "$AK3/anykernel.sh"

    # Kernel only.  The stock boot.img carries no appended DTB and ak3-core.sh
    # silently appends any dtb/dtbo found in the zip root, which changes how
    # the panel initialises.  Keep the layout stock-shaped.
    cp -f "$outd/Image" "$AK3/"

    local stamp
    stamp="$(date -u +%Y%m%d-%H%M)"
    local zip_file="${ZIP_NAME}-${stamp}.zip"

    ( cd "$AK3" && rm -f ./*.zip && zip -x "*.zip" -r9 "$zip_file" * )
    mv "$AK3/$zip_file" "$KERNEL_DIR/"
    echo "=== Done: $KERNEL_DIR/$zip_file ==="
}

if [ "${1:-}" = "pack" ]; then
    package
else
    build_kernel
    package
fi
