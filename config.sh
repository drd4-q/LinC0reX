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
    # 1. Имя и базовая плавность
    echo 'CONFIG_LOCALVERSION="-LinC0reX"'
    echo "CONFIG_HZ_300=y"
    echo "CONFIG_HZ=300"
    echo "CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y"

    # 2. Оптимизация ОЗУ (MGLRU + KSM)
    echo "CONFIG_LRU_GEN=y"
    echo "CONFIG_LRU_GEN_ENABLED=y"
    echo "CONFIG_KSM=y"
    echo "CONFIG_MEMORY_FAILURE=y"

    # 3. Поддержка Kexec (Запуск ОС без перезагрузки)
    echo "CONFIG_KEXEC=y"
    echo "CONFIG_KEXEC_CORE=y"
    echo "CONFIG_KEXEC_FILE=n"
    echo "CONFIG_PROC_KCORE=y"
    echo "CONFIG_KALLSYMS=y"
    echo "CONFIG_KALLSYMS_ALL=y"

    # 4. Docker и Контейнеры (Все необходимые контроллеры)
    echo "CONFIG_NAMESPACES=y"
    echo "CONFIG_USER_NS=y"
    echo "CONFIG_CGROUPS=y"
    echo "CONFIG_MEMCG=y"
    echo "CONFIG_MEMCG_SWAP=y"
    echo "CONFIG_MEMCG_SWAP_ENABLED=y"
    echo "CONFIG_CFS_BANDWIDTH=y"
    echo "CONFIG_CGROUP_PIDS=y"
    echo "CONFIG_CGROUP_FREEZER=y"
    echo "CONFIG_VETH=y"
    echo "CONFIG_BRIDGE=y"
    echo "CONFIG_OVERLAY_FS=y"
    echo "CONFIG_ANDROID_BINDERFS=y"

    # 5. Сеть и VPN
    echo "CONFIG_WIREGUARD=y"
    echo "CONFIG_TCP_CONG_BBR=y"
    echo "CONFIG_DEFAULT_BBR=y"

    # 6. Фикс графики (Чтобы экран не зависал)
    echo "CONFIG_DRM_MSM=n"
    echo "CONFIG_FB=y"
    echo "CONFIG_DRM_FBDEV_EMULATION=y"
    echo "CONFIG_FRAMEBUFFER_CONSOLE=n"
    echo "CONFIG_VT=n"

    # 7. Параметры загрузки
    echo 'CONFIG_CMDLINE="kvm-arm.mode=none androidboot.hypervisor=0 nokaslr reset_devices"'
    echo "CONFIG_CMDLINE_EXTEND=y"

    # 8. Фиксы Qualcomm и Alpine
    echo "CONFIG_ARM_MEMLAT_MON=n"
    echo "CONFIG_DEVFREQ_GOV_MEMLAT=n"
    echo "CONFIG_PROFILING=y"
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
