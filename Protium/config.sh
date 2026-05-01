#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-2.0-only

# Compare kernel versions in order to apply the correct patches
version_le() {
    [ "$(printf '%s\n' "$1" "$2" | sort -V | head -n1)" = "$1" ]
}

# Avoid dirty uname
touch $KERNEL_DIR/.scmversion

msg "KernelSU"
#cd $KERNEL_DIR && curl https://raw.githubusercontent.com/$KERNELSU_REPO/refs/heads/master/kernel/setup.sh | bash -s $KERNELSU_BRANCH
msg "Importing KernelSU..."

#cd $KERNEL_DIR/KernelSU && curl https://raw.githubusercontent.com/$BUILDER_REPO/refs/heads/$BUILDER_BRANCH/patches/ksu/no_dummy_keystore.patch | git am

cd $KERNEL_DIR

echo "CONFIG_KSU=y" >> $DEVICE_DEFCONFIG_FILE
echo "CONFIG_KSU_EXTRAS=y" >> $DEVICE_DEFCONFIG_FILE
echo "CONFIG_KSU_TAMPER_SYSCALL_TABLE=y" >> $DEVICE_DEFCONFIG_FILE
echo "CONFIG_KSU_THRONE_TRACKER_ALWAYS_THREADED=y" >> $DEVICE_DEFCONFIG_FILE
echo "CONFIG_KPROBES=n" >> $DEVICE_DEFCONFIG_FILE # it will conflict with KSU hooks if it's on

KSU_GIT_VERSION=$(cd KernelSU && git rev-list --count HEAD)
KERNELSU_VERSION=$(grep -r "DKSU_VERSION" $KERNEL_DIR/KernelSU/kernel/Makefile | cut -d '=' -f3)

msg "KernelSU Version: $KERNELSU_VERSION"

# --- DOCKER, LXC AND RAM OPTIMIZATIONS ---
msg "Adding Docker and RAM optimizations..."
{
    # 1. Имя и базовая оптимизация
    echo 'CONFIG_LOCALVERSION="-LinC0reX-OwO"'
    echo "CONFIG_HZ_300=y"
    echo "CONFIG_HZ=300"
    echo "CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y"

    # 2. LLVM LTO (Link-Time Optimization)
    echo "CONFIG_LTO_CLANG=y"
    echo "CONFIG_LTO_CLANG_THIN=y"

    # 3. KEXEC (Запуск ОС из-под Android. Только CORE, без FILE чтобы не было ошибок FDT)
    echo "CONFIG_KEXEC=y"
    echo "CONFIG_KEXEC_CORE=y"
    echo "CONFIG_KEXEC_FILE=n"
    echo "CONFIG_KVM=n"
    echo 'CONFIG_CMDLINE="kvm-arm.mode=none androidboot.hypervisor=0"'
    echo "CONFIG_CMDLINE_EXTEND=y"

    echo "CONFIG_WIREGUARD=y" >> $DEVICE_DEFCONFIG_FILE
    echo "CONFIG_CRYPTO_CHACHA20POLY1305=y" >> $DEVICE_DEFCONFIG_FILE
    echo "CONFIG_CRYPTO_POLY1305=y" >> $DEVICE_DEFCONFIG_FILE
    echo "CONFIG_KEXEC=y" >> $DEVICE_DEFCONFIG_FILE
    echo "CONFIG_KEXEC_CORE=y" >> $DEVICE_DEFCONFIG_FILE

    # 6. Оптимизация ОЗУ и Сети
    echo "CONFIG_LRU_GEN=y"
    echo "CONFIG_LRU_GEN_ENABLED=y"
    echo "CONFIG_TCP_CONG_BBR=y"
    echo "CONFIG_DEFAULT_BBR=y"
    echo "CONFIG_IOSCHED_MAPLE=y"
    echo "CONFIG_DEFAULT_MAPLE=y"

    # 7. Docker & LXC (Все контроллеры)
    echo "CONFIG_NAMESPACES=y"
    echo "CONFIG_USER_NS=y"
    echo "CONFIG_CGROUPS=y"
    echo "CONFIG_MEMCG=y"
    echo "CONFIG_MEMCG_SWAP=y"
    echo "CONFIG_CFS_BANDWIDTH=y"
    echo "CONFIG_VETH=y"
    echo "CONFIG_BRIDGE=y"
    echo "CONFIG_OVERLAY_FS=y"
    echo "CONFIG_ANDROID_BINDERFS=y"

    # 8. Отключение KVM и pKVM (Чтобы работал Kexec на Android 15)
    echo "CONFIG_KVM=n"
    echo "CONFIG_VIRTUALIZATION=n"
    echo 'CONFIG_CMDLINE="kvm-arm.mode=none androidboot.hypervisor=0"'
    echo "CONFIG_CMDLINE_EXTEND=y"

    # 9. Фиксы Qualcomm и Дисплея (чтобы не было пикселей)
    echo "CONFIG_ARM_MEMLAT_MON=n"
    echo "CONFIG_DEVFREQ_GOV_MEMLAT=n"
    echo "CONFIG_PROFILING=y"
    echo "CONFIG_VT=n"
    echo "CONFIG_FB=n"
    echo "CONFIG_DRM_FBDEV_EMULATION=n"

    # 10. Файловые системы
    echo "CONFIG_NTFS_FS=y"
    echo "CONFIG_EXFAT_FS=y"

    # 11. Жесткое удаление Alpine (чистим путь)
    echo 'CONFIG_INITRAMFS_SOURCE=""'
} >> $DEVICE_DEFCONFIG_FILE



if [[ $VB_ENABLED == "true" ]]; then
    msg "VB"
fi
if [[ $VB_ENABLED == "false" ]]; then
    msg "NonVB"
#curl https://raw.githubusercontent.com/$BUILDER_REPO/refs/heads/$BUILDER_BRANCH/patches/initramfs_recovery.patch | git am
fi
CONFIG_INITRAMFS_SOURCE=""
