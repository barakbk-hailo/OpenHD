#!/bin/bash
################################################################################
# OpenHD Native Build Script (RPi4 / RPi5 / x86)
#
# Run this directly on the target machine. Source repos are expected as sibling
# directories alongside this repo:
#   <parent>/OpenHD/          (this repo)
#   <parent>/OpenHD-SysUtils/ (SysUtils repo)
#
# Usage:
#   sudo ./build_rpi5_native.sh [options] <command>
#
# Commands:
#   deps    - Install build + runtime dependencies
#   build   - Build SysUtils + OpenHD, install binaries
#   driver  - Build + install WiFi driver (rtl88x2bu)
#   all     - deps + build + driver
#
# Options:
#   --enable-service  Install and enable systemd services (default: off)
################################################################################

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PARENT_DIR="$(dirname "$SCRIPT_DIR")"
SYSUTILS_DIR="$PARENT_DIR/OpenHD-SysUtils"
DRIVER_BUILD_DIR="$PARENT_DIR/rtl88x2bu_build"
ENABLE_SERVICE=false
SYSTEMD_DIR="/etc/systemd/system"

# Detect platform
ARCH="$(uname -m)"
if [ -f /proc/device-tree/model ]; then
    MODEL=$(tr -d '\0' < /proc/device-tree/model)
    case "$MODEL" in
        *"Raspberry Pi 5"*) PLATFORM="rpi5" ;;
        *"Raspberry Pi 4"*) PLATFORM="rpi4" ;;
        *"Raspberry Pi"*)   PLATFORM="rpi"  ;;
    esac
elif [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "i686" ]; then
    PLATFORM="x86"
else
    echo "Warning: Unknown platform '$ARCH', assuming x86"
    PLATFORM="x86"
fi
echo "Detected platform: $PLATFORM ($ARCH)"

# Parse options
while [[ $# -gt 0 ]]; do
    case "$1" in
        --enable-service) ENABLE_SERVICE=true; shift ;;
        -*) echo "Unknown option: $1"; exit 1 ;;
        *) break ;;
    esac
done

CMD="${1:-}"

cmd_deps() {
    echo "=== Installing build + runtime dependencies ==="
    cd "$SCRIPT_DIR"

    # Build dependencies
    case "$PLATFORM" in
        rpi5)       ./install_build_dep.sh rpi5 ;;
        rpi4|rpi)   ./install_build_dep.sh rpi ;;
        x86)        ./install_build_dep.sh ubuntu-x86 ;;
    esac

    # Runtime dependencies
    apt-get install -y -o Dpkg::Options::='--force-overwrite' --no-install-recommends \
        iw nmap aircrack-ng i2c-tools \
        gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
        gstreamer1.0-tools gstreamer1.0-alsa \
        || true

    mkdir -p /usr/local/share/openhd/
    if [ ! -f /usr/local/share/openhd/joyconfig.txt ]; then
        touch /usr/local/share/openhd/joyconfig.txt
    fi

    echo "=== Dependencies installed ==="
}

cmd_build() {
    echo "=== Building OpenHD ==="

    # Build SysUtils first (OpenHD depends on it at runtime)
    if [ -d "$SYSUTILS_DIR" ]; then
        echo ""
        echo "--- Compiling SysUtils ---"
        cd "$SYSUTILS_DIR"
        cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
        cmake --build build_release -j$(nproc)
        cp build_release/openhd_sys_utils /usr/local/bin/openhd_sys_utils
        chmod +x /usr/local/bin/openhd_sys_utils

        if [ "$ENABLE_SERVICE" = true ]; then
            cp systemd/openhd-sys-utils.service "$SYSTEMD_DIR/"
            systemctl enable openhd-sys-utils.service 2>/dev/null || true
            echo "SysUtils service installed and enabled."
        fi
        echo "SysUtils installed."
    else
        echo "Warning: $SYSUTILS_DIR not found, skipping SysUtils."
    fi

    echo ""
    echo "--- Compiling OpenHD ---"
    cd "$SCRIPT_DIR/OpenHD"
    cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
    cmake --build build_release -j$(nproc)
    cp build_release/openhd /usr/local/bin/openhd
    chmod +x /usr/local/bin/openhd

    if [ "$ENABLE_SERVICE" = true ]; then
        if [ "$PLATFORM" = "rpi5" ] || [ "$PLATFORM" = "rpi4" ] || [ "$PLATFORM" = "rpi" ]; then
            cp "$SCRIPT_DIR/systemd/openhd_rpi.service" "$SYSTEMD_DIR/openhd.service"
        else
            cp "$SCRIPT_DIR/systemd/openhd.service" "$SYSTEMD_DIR/openhd.service"
        fi
        systemctl enable openhd.service 2>/dev/null || true
        echo "OpenHD service installed and enabled."
    fi

    # Install default config only if not already present
    mkdir -p /boot/openhd
    if [ ! -f /boot/openhd/hardware.config ]; then
        cp "$SCRIPT_DIR/OpenHD/ohd_common/config/hardware.config" /boot/openhd/hardware.config
        echo "Default hardware.config installed."
    else
        echo "Existing hardware.config preserved."
    fi

    echo ""
    echo "=== Build complete! Binary installed to /usr/local/bin/openhd ==="
}

cmd_driver() {
    echo "=== Building WiFi driver (rtl88x2bu via DKMS) ==="

    # Kernel headers + dkms so the module auto-rebuilds on every kernel upgrade
    # (without DKMS, an apt upgrade that bumps the kernel silently breaks WFB).
    apt-get install -y --no-install-recommends linux-headers-$(uname -r) dkms || true

    # Source must live in /usr/src/<package>-<version>/ for DKMS to find it
    # again when a new kernel arrives. AUTOINSTALL=yes in the upstream
    # dkms.conf takes care of the post-kernel-upgrade rebuild.
    local VERSION="5.13.1-ohd"
    local SRC_DIR="/usr/src/rtl88x2bu-${VERSION}"
    local REPO_URL
    if [ "$PLATFORM" = "rpi5" ] || [ "$PLATFORM" = "rpi4" ] || [ "$PLATFORM" = "rpi" ]; then
        REPO_URL="https://github.com/barakbk-hailo/rtl88x2bu.git"
    else
        REPO_URL="https://github.com/OpenHD/rtl88x2bu.git"
    fi

    # Idempotent: drop any previous DKMS registration before re-adding.
    dkms status -m rtl88x2bu 2>/dev/null | awk -F'[ ,/]' '/rtl88x2bu/{print $2}' | sort -u | while read v; do
        [ -n "$v" ] && dkms remove "rtl88x2bu/$v" --all 2>/dev/null || true
    done

    rm -rf "$SRC_DIR" "$DRIVER_BUILD_DIR"
    git clone "$REPO_URL" "$SRC_DIR"
    # Upstream dkms.conf has @PKGVER@ as a sed-substitution placeholder.
    sed -i "s/@PKGVER@/${VERSION}/" "$SRC_DIR/dkms.conf"

    dkms add "rtl88x2bu/${VERSION}"
    # --force: overwrite any stale 88x2bu_ohd.ko left behind by a previous
    # non-DKMS install. Same major version + git-SHA suffix means DKMS can't
    # compare them as monotonic versions and refuses without --force.
    dkms install --force "rtl88x2bu/${VERSION}"
    depmod -a

    # Blacklist stock driver so our _ohd version loads
    echo "blacklist rtw88_8822bu" > /etc/modprobe.d/rtw8822bu.conf

    echo ""
    echo "=== rtl88x2bu_ohd installed via DKMS — auto-rebuilds on kernel upgrades ==="
    dkms status -m rtl88x2bu 2>&1 | sed 's/^/  /'
}

cmd_all() {
    cmd_deps
    cmd_build
    cmd_driver
}

# ---- Main ----

case "$CMD" in
    deps)    cmd_deps ;;
    build)   cmd_build ;;
    driver)  cmd_driver ;;
    all)     cmd_all ;;
    *)
        echo "Usage: sudo $0 [options] <command>"
        echo ""
        echo "Commands:"
        echo "  deps    - Install build + runtime dependencies"
        echo "  build   - Build SysUtils + OpenHD, install binaries"
        echo "  driver  - Build + install WiFi driver (rtl88x2bu)"
        echo "  all     - deps + build + driver"
        echo ""
        echo "Options:"
        echo "  --enable-service  Install and enable systemd services (default: off)"
        echo ""
        echo "Platform detected: $PLATFORM ($ARCH)"
        exit 1
        ;;
esac
