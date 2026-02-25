#!/bin/bash
################################################################################
# OpenHD RPI5 Native Build Script
#
# Run this directly on the Raspberry Pi 5. Source repos should be at:
#   /opt/OpenHD/          (this repo)
#   /opt/OpenHD-SysUtils/ (SysUtils repo)
#
# Usage:
#   sudo ./build_rpi5_native.sh deps    - Install build + runtime dependencies
#   sudo ./build_rpi5_native.sh build   - Build SysUtils + OpenHD, install binaries
#   sudo ./build_rpi5_native.sh driver  - Build + install WiFi driver (rtl88x2bu)
#   sudo ./build_rpi5_native.sh all     - deps + build + driver
################################################################################

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SYSUTILS_DIR="/opt/OpenHD-SysUtils"

cmd_deps() {
    echo "=== Installing build + runtime dependencies ==="
    cd "$SCRIPT_DIR"

    # Build dependencies
    ./install_build_dep.sh rpi5

    # Runtime dependencies
    apt-get install -y -o Dpkg::Options::='--force-overwrite' --no-install-recommends \
        iw nmap aircrack-ng i2c-tools \
        gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
        gstreamer1.0-tools gstreamer1.0-alsa \
        || true

    mkdir -p /usr/local/share/openhd/
    touch /usr/local/share/openhd/joyconfig.txt

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
        cp systemd/openhd-sys-utils.service /lib/systemd/system/
        systemctl enable openhd-sys-utils.service 2>/dev/null || true
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

    cp "$SCRIPT_DIR/systemd/openhd_rpi.service" /etc/systemd/system/openhd.service
    systemctl enable openhd.service 2>/dev/null || true

    echo ""
    echo "=== Build complete! Binary installed to /usr/local/bin/openhd ==="
}

cmd_driver() {
    echo "=== Building WiFi driver (rtl88x2bu) ==="

    # Need kernel headers
    apt-get install -y --no-install-recommends linux-headers-$(uname -r) || true

    local DRIVER_DIR="/tmp/rtl88x2bu_build"
    rm -rf "$DRIVER_DIR"
    git clone https://github.com/barakbk-hailo/rtl88x2bu.git "$DRIVER_DIR"
    cd "$DRIVER_DIR"
    make -j$(nproc)
    make install
    depmod -a

    # Blacklist stock driver so our _ohd version loads
    echo "blacklist rtw88_8822bu" > /etc/modprobe.d/rtw8822bu.conf

    rm -rf "$DRIVER_DIR"
    echo ""
    echo "=== WiFi driver installed. Reboot for it to load. ==="
}

cmd_all() {
    cmd_deps
    cmd_build
    cmd_driver
}

# ---- Main ----

CMD="${1:-}"

case "$CMD" in
    deps)    cmd_deps ;;
    build)   cmd_build ;;
    driver)  cmd_driver ;;
    all)     cmd_all ;;
    *)
        echo "Usage: sudo $0 <command>"
        echo ""
        echo "Commands:"
        echo "  deps    - Install build + runtime dependencies"
        echo "  build   - Build SysUtils + OpenHD, install binaries + services"
        echo "  driver  - Build + install WiFi driver (rtl88x2bu)"
        echo "  all     - deps + build + driver"
        exit 1
        ;;
esac
