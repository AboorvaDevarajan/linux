#!/bin/bash
# Build and deploy the BTT debug kernel
#
# Prerequisites:
#   - config.1 has been copied to .config
#   - btt-trace-debug.patch has been applied (or btt.c already modified)
#   - You are on the debug_pmem_bug branch
#
# Usage:
#   ./repro/build_and_deploy.sh          # build + install
#   ./repro/build_and_deploy.sh build    # build only
#   ./repro/build_and_deploy.sh install  # install only (after build)

set -e

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
LOCALVER="-bttdebug"
NPROC=$(nproc)

cd "$SRCDIR"

do_build() {
    echo "=== Configuring kernel ==="
    if [ ! -f .config ]; then
        if [ -f config.1 ]; then
            cp config.1 .config
        else
            echo "ERROR: No .config or config.1 found"
            exit 1
        fi
    fi

    # Override localversion to distinguish this build
    sed -i "s/^CONFIG_LOCALVERSION=.*/CONFIG_LOCALVERSION=\"${LOCALVER}\"/" .config
    make olddefconfig

    echo "=== Building kernel (${NPROC} jobs) ==="
    make -j"${NPROC}" vmlinux modules 2>&1 | tail -5

    echo "=== Kernel version ==="
    make kernelrelease
}

do_install() {
    KREL=$(make -s kernelrelease)
    echo "=== Installing kernel ${KREL} ==="

    # Install modules
    sudo make modules_install

    # Install kernel image
    sudo make install

    echo ""
    echo "=== Done ==="
    echo "Kernel installed as: ${KREL}"
    echo ""
    echo "To boot into it:"
    echo "  sudo grubby --set-default /boot/vmlinuz-${KREL}"
    echo "  sudo reboot"
    echo ""
    echo "After booting, verify:"
    echo "  uname -r   # should show ${KREL}"
}

case "${1:-all}" in
    build)   do_build ;;
    install) do_install ;;
    all)     do_build; do_install ;;
    *)       echo "Usage: $0 [build|install|all]"; exit 1 ;;
esac
