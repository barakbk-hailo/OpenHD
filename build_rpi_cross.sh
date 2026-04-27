#!/bin/bash
################################################################################
# OpenHD RPI5 Cross-Build Script
#
# Usage:
#   sudo ./build_rpi_cross.sh setup    - First time: download image, mount, install deps
#   sudo ./build_rpi_cross.sh build    - Rebuild: sync source + compile (fast iteration)
#   sudo ./build_rpi_cross.sh image    - Save flashable .img file
#   sudo ./build_rpi_cross.sh shell    - Drop into chroot shell for debugging
#   sudo ./build_rpi_cross.sh unmount  - Unmount everything when done
#   sudo ./build_rpi_cross.sh all      - Full pipeline (setup + build + image + unmount)
################################################################################

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SYSUTILS_DIR="$(cd "$SCRIPT_DIR/../OpenHD-SysUtils" 2>/dev/null && pwd)" || SYSUTILS_DIR=""
cd "$SCRIPT_DIR"

IMAGE_DIR="$SCRIPT_DIR/.rpi-build"
IMAGE_URL="https://downloads.raspberrypi.org/raspios_lite_arm64/images/raspios_lite_arm64-2025-05-13/2025-05-13-raspios-bookworm-arm64-lite.img.xz"
IMAGE_FILE="$IMAGE_DIR/raspios.img"
OUTPUT_IMAGE="$SCRIPT_DIR/openhd-rpi5.img"
MOUNT_DIR="$IMAGE_DIR/mnt"

# ---- Helper functions ----

is_mounted() {
    mountpoint -q "$MOUNT_DIR" 2>/dev/null
}

get_loop_dev() {
    losetup -j "$IMAGE_FILE" 2>/dev/null | cut -d: -f1 | head -1
}

do_unmount() {
    echo "--- Unmounting ---"
    sudo umount "$MOUNT_DIR/dev/pts" 2>/dev/null || true
    sudo umount "$MOUNT_DIR/dev" 2>/dev/null || true
    sudo umount "$MOUNT_DIR/proc" 2>/dev/null || true
    sudo umount "$MOUNT_DIR/sys" 2>/dev/null || true
    sudo umount "$MOUNT_DIR/boot" 2>/dev/null || true
    sudo umount "$MOUNT_DIR" 2>/dev/null || true
    local loop=$(get_loop_dev)
    [ -n "$loop" ] && sudo losetup -d "$loop" 2>/dev/null || true
    echo "Done."
}

do_mount() {
    if is_mounted; then
        echo "Already mounted at $MOUNT_DIR"
        return
    fi

    if [ ! -f "$IMAGE_FILE" ]; then
        echo "Error: No image file. Run 'setup' first."
        exit 1
    fi

    echo "--- Mounting image ---"
    sudo mkdir -p "$MOUNT_DIR"
    LOOP_DEV=$(sudo losetup --show -fP "$IMAGE_FILE")
    echo "Loop device: $LOOP_DEV"
    sudo e2fsck -fy "${LOOP_DEV}p2" || true
    sudo resize2fs "${LOOP_DEV}p2" 2>/dev/null || true
    sudo mount "${LOOP_DEV}p2" "$MOUNT_DIR"
    sudo mount "${LOOP_DEV}p1" "$MOUNT_DIR/boot"

    # QEMU + bind mounts
    sudo cp /usr/bin/qemu-aarch64-static "$MOUNT_DIR/usr/bin/"
    sudo mount --bind /dev "$MOUNT_DIR/dev"
    sudo mount --bind /dev/pts "$MOUNT_DIR/dev/pts"
    sudo mount --bind /proc "$MOUNT_DIR/proc"
    sudo mount --bind /sys "$MOUNT_DIR/sys"
    sudo cp /etc/resolv.conf "$MOUNT_DIR/etc/resolv.conf"
    echo "Mounted."
}

do_sync_source() {
    echo "--- Syncing OpenHD source into chroot ---"
    sudo mkdir -p "$MOUNT_DIR/opt/OpenHD"
    sudo rsync -a --delete \
        --exclude='.rpi-build' \
        --exclude='.git' \
        --exclude='openhd-arm' \
        --exclude='openhd-rpi5.img' \
        --exclude='**/build_release' \
        "$SCRIPT_DIR/" "$MOUNT_DIR/opt/OpenHD/"

    if [ -n "$SYSUTILS_DIR" ] && [ -d "$SYSUTILS_DIR" ]; then
        echo "--- Syncing SysUtils source into chroot ---"
        sudo mkdir -p "$MOUNT_DIR/opt/OpenHD-SysUtils"
        sudo rsync -a --delete \
            --exclude='.git' \
            --exclude='**/build_release' \
            "$SYSUTILS_DIR/" "$MOUNT_DIR/opt/OpenHD-SysUtils/"
    fi
    echo "Done."
}

# ---- Commands ----

cmd_setup() {
    echo "=== Setup: Download image, mount, install dependencies ==="

    # Host tools
    echo ""
    echo "--- Installing host tools ---"
    sudo apt-get update -qq
    sudo apt-get install -y -qq qemu-user-static binfmt-support xz-utils parted e2fsprogs rsync wget

    # Download image
    mkdir -p "$IMAGE_DIR"
    if [ ! -f "$IMAGE_DIR/raspios-base.img" ]; then
        echo ""
        echo "--- Downloading Raspberry Pi OS image ---"
        wget -O "$IMAGE_FILE.xz" "$IMAGE_URL"
        echo "Decompressing..."
        xz -d "$IMAGE_FILE.xz"
        cp "$IMAGE_FILE" "$IMAGE_DIR/raspios-base.img"
    else
        echo "--- Using cached Raspberry Pi OS image ---"
        cp "$IMAGE_DIR/raspios-base.img" "$IMAGE_FILE"
    fi

    # Expand image
    echo ""
    echo "--- Expanding image (+4GB) ---"
    truncate -s +4G "$IMAGE_FILE"
    PART_START=$(parted -s "$IMAGE_FILE" unit s print | awk '/^ 2/{print $2}' | tr -d 's')
    parted -s "$IMAGE_FILE" rm 2
    parted -s "$IMAGE_FILE" mkpart primary ext4 "${PART_START}s" 100%

    # Mount
    do_mount

    # Copy source
    do_sync_source

    # Install dependencies inside chroot
    echo ""
    echo "--- Installing build dependencies (this takes a while the first time) ---"
    sudo chroot "$MOUNT_DIR" /bin/bash -c "
        set -e
        echo 'Architecture:' \$(uname -m)
        apt-get update
        cd /opt/OpenHD
        mkdir -p /usr/local/share/openhd/
        touch /usr/local/share/openhd/joyconfig.txt
        ./install_build_dep.sh rpi5

        # Also install runtime deps
        apt-get install -y -o Dpkg::Options::='--force-overwrite' --no-install-recommends \
            iw nmap aircrack-ng i2c-tools \
            gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
            gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
            gstreamer1.0-tools gstreamer1.0-alsa \
            || true
    "

    # Build and install WiFi driver
    echo ""
    echo "--- Building WiFi driver (rtl88x2bu) ---"
    sudo chroot "$MOUNT_DIR" /bin/bash -c "
        set -e
        apt-get install -y --no-install-recommends dkms linux-headers-\$(uname -r) || true
        git clone -b master-hailo https://github.com/giladnah/rtl88x2bu.git /tmp/rtl88x2bu
        cd /tmp/rtl88x2bu
        make -j\$(nproc)
        make install
        depmod -a
        echo 'blacklist rtw88_8822bu' > /etc/modprobe.d/rtw8822bu.conf
        rm -rf /tmp/rtl88x2bu
        echo 'WiFi driver installed.'
    "

    echo ""
    echo "=== Setup complete! Image is mounted at $MOUNT_DIR ==="
    echo "Now run: sudo ./build_rpi_cross.sh build"
}

cmd_build() {
    echo "=== Build: Sync source + compile ==="

    do_mount
    do_sync_source

    # Build SysUtils first (OpenHD depends on it at runtime)
    if [ -d "$MOUNT_DIR/opt/OpenHD-SysUtils" ]; then
        echo ""
        echo "--- Compiling SysUtils ---"
        sudo chroot "$MOUNT_DIR" /bin/bash -c "
            set -e
            cd /opt/OpenHD-SysUtils
            cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
            cmake --build build_release -j\$(nproc)

            # Install binary + service
            cp build_release/openhd_sys_utils /usr/local/bin/openhd_sys_utils
            chmod +x /usr/local/bin/openhd_sys_utils
            cp systemd/openhd-sys-utils.service /lib/systemd/system/
            systemctl enable openhd-sys-utils.service 2>/dev/null || true

            echo 'SysUtils build complete!'
        "
    fi

    echo ""
    echo "--- Compiling OpenHD ---"
    sudo chroot "$MOUNT_DIR" /bin/bash -c "
        set -e
        cd /opt/OpenHD/OpenHD
        cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
        cmake --build build_release -j\$(nproc)

        # Install binary
        cp build_release/openhd /usr/local/bin/openhd
        chmod +x /usr/local/bin/openhd

        # Install service
        cp /opt/OpenHD/systemd/openhd_rpi.service /etc/systemd/system/openhd.service
        systemctl enable openhd.service 2>/dev/null || true

        echo ''
        echo 'Build complete! Binary installed to /usr/local/bin/openhd'
    "

    # Copy binaries out for convenience
    sudo cp "$MOUNT_DIR/usr/local/bin/openhd" "$SCRIPT_DIR/openhd-arm"
    sudo chown $(id -u):$(id -g) "$SCRIPT_DIR/openhd-arm"
    if [ -f "$MOUNT_DIR/usr/local/bin/openhd_sys_utils" ]; then
        sudo cp "$MOUNT_DIR/usr/local/bin/openhd_sys_utils" "$SCRIPT_DIR/openhd_sys_utils-arm"
        sudo chown $(id -u):$(id -g) "$SCRIPT_DIR/openhd_sys_utils-arm"
    fi
    echo ""
    echo "Binaries copied to: openhd-arm, openhd_sys_utils-arm"
}

cmd_image() {
    echo "=== Image: Saving flashable .img ==="

    if ! is_mounted; then
        echo "Error: Not mounted. Run 'setup' or 'build' first."
        exit 1
    fi

    echo "--- Copying image (mount stays active) ---"
    cp "$IMAGE_FILE" "$OUTPUT_IMAGE"

    echo ""
    echo "=== Flashable image saved: openhd-rpi5.img ($(du -h "$OUTPUT_IMAGE" | cut -f1)) ==="
    echo ""
    echo "Flash with:"
    echo "  Linux/WSL:  sudo dd if=openhd-rpi5.img of=/dev/sdX bs=4M status=progress"
    echo "  Windows:    Use Raspberry Pi Imager or balenaEtcher"
}

cmd_shell() {
    echo "=== Shell: Entering chroot (Ctrl+D to exit) ==="
    do_mount
    sudo chroot "$MOUNT_DIR" /bin/bash
}

cmd_unmount() {
    do_unmount
}

cmd_all() {
    cmd_setup
    cmd_build
    cmd_image
}

# ---- Main ----

CMD="${1:-}"

case "$CMD" in
    setup)   cmd_setup ;;
    build)   cmd_build ;;
    image)   cmd_image ;;
    shell)   cmd_shell ;;
    unmount) cmd_unmount ;;
    all)     cmd_all ;;
    *)
        echo "Usage: sudo $0 <command>"
        echo ""
        echo "Commands:"
        echo "  setup    - First time: download image, mount, install deps"
        echo "  build    - Rebuild: sync source + compile (fast iteration)"
        echo "  image    - Save flashable openhd-rpi5.img (unmounts)"
        echo "  shell    - Drop into chroot shell for debugging"
        echo "  unmount  - Unmount everything"
        echo "  all      - Full pipeline: setup + build + image"
        echo ""
        echo "Typical workflow:"
        echo "  sudo $0 setup          # once"
        echo "  sudo $0 build          # after each code change"
        echo "  sudo $0 shell          # to debug inside the chroot"
        echo "  sudo $0 image          # when ready to flash"
        exit 1
        ;;
esac
