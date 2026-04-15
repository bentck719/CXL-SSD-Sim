#!/usr/bin/env bash

GEM5_OPT="build/X86/gem5.opt"
KERNEL_PATH="/home/ben/io-exp/kernel-observation/linux-6.8/vmlinux"
DISK_IMAGE="/home/ben/io-exp/simulation/qemu-kvm/images/vm_root.img"
FS_CONFIG="configs/example/fs.py"
CUSTOM_CMDLINE="earlyprintk=ttyS0 console=ttyS0 lpj=7999923 root=/dev/sda rootfstype=ext4 rw rootwait early_page_ext systemd.mask=boot-efi.mount systemd.mask=systemd-networkd-wait-online.service multipath=off"

echo "🚀 準備啟動 COBRA Ping-Pong 測試..."
echo "📦 Kernel 路徑：$KERNEL_PATH"
echo "💡 模擬器啟動後，請開啟另一個終端機使用 'm5term 3456' (或對應的 port) 連接 Guest OS。"
echo ""

$GEM5_OPT \
    --debug-flags=CxlMemory \
    $FS_CONFIG \
    --kernel=$KERNEL_PATH \
    --disk-image=$DISK_IMAGE \
    --num-cpus=1 \
    --cpu-type=X86KvmCPU \
    --mem-size=3GB \
    --command-line="$CUSTOM_CMDLINE"
