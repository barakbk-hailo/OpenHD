#!/bin/bash
################################################################################
# OpenHD Native Build Script (RPi5 / x86)
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
case "$ARCH" in
    aarch64|armv7l) PLATFORM="rpi" ;;
    x86_64|i686)    PLATFORM="x86" ;;
    *)
        echo "Warning: Unknown architecture '$ARCH', assuming x86"
        PLATFORM="x86"
        ;;
esac
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

    # Build dependencies (install_build_dep.sh uses "rpi5" or "ubuntu-x86")
    if [ "$PLATFORM" = "rpi" ]; then
        ./install_build_dep.sh rpi5
    else
        ./install_build_dep.sh ubuntu-x86
    fi

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
        if [ "$PLATFORM" = "rpi" ]; then
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
    echo "=== Building WiFi driver (rtl88x2bu) ==="

    # Need kernel headers
    apt-get install -y --no-install-recommends linux-headers-$(uname -r) || true

    rm -rf "$DRIVER_BUILD_DIR"

    if [ "$PLATFORM" = "rpi" ]; then
        git clone https://github.com/barakbk-hailo/rtl88x2bu.git "$DRIVER_BUILD_DIR"
    else
        git clone https://github.com/OpenHD/rtl88x2bu.git "$DRIVER_BUILD_DIR"
    fi

    cd "$DRIVER_BUILD_DIR"
    make -j$(nproc)
    make install
    depmod -a

    # Blacklist stock driver so our _ohd version loads
    echo "blacklist rtw88_8822bu" > /etc/modprobe.d/rtw8822bu.conf

    rm -rf "$DRIVER_BUILD_DIR"
    echo ""
    echo "=== WiFi driver installed. Reboot for it to load. ==="
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
