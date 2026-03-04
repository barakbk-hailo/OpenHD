# OpenHD + Hailo AI Drone-Follow Integration

Comprehensive documentation for the Hailo AI autonomous drone-following system built across three repositories:

- **OpenHD** — Video link firmware (air + ground units)
- **QOpenHD** — Ground station UI (Qt/QML)
- **hailo-drone-follow** — AI person detection + autonomous flight control

> **Base version:** OpenHD **2.6-evo** (branch `2.6-evo-hailo`). All Hailo integration work is on the `feature/hailo-apps-integration` branch.

---

## Table of Contents

1. [System Architecture](#1-system-architecture)
2. [Two Hailo Operating Modes](#2-two-hailo-operating-modes)
3. [Pipeline Overview](#3-pipeline-overview)
4. [OpenHD Configuration Reference](#4-openhd-configuration-reference)
5. [Build & Run Instructions](#5-build--run-instructions)
6. [Hailo Bridge Parameters](#6-hailo-bridge-parameters)
7. [Binary Detection Payload v3](#7-binary-detection-payload-v3)
8. [QOpenHD Detection Display](#8-qopenhd-detection-display)
9. [Key Technical Details](#9-key-technical-details)
10. [High-Level Changelog (Final State)](#10-high-level-changelog-final-state)
11. [Detailed Changelog — OpenHD](#11-detailed-changelog--openhd)
12. [Detailed Changelog — QOpenHD](#12-detailed-changelog--qopenhd)
13. [Detailed Changelog — hailo-drone-follow](#13-detailed-changelog--hailo-drone-follow)
---

## 1. System Architecture

### High-Level Overview

```
┌─────────────────────────────────────────────────────────┐
│                     AIR UNIT                             │
│  Raspberry Pi 5 + Hailo-8L + CSI Camera + Flight Ctrl   │
│                                                         │
│  ┌──────────────┐   ┌──────────────┐   ┌─────────────┐ │
│  │ drone-follow │   │   OpenHD     │   │   MAVLink    │ │
│  │  (Hailo AI)  │◄─►│  Air Process │◄─►│   FC (PX4)  │ │
│  └──────┬───────┘   └──────┬───────┘   └─────────────┘ │
│         │                  │                             │
│         │  UDP params      │  Video + Telemetry          │
│         │  5510/5511       │  + Detection data           │
│         │  SHM /tmp/...    │                             │
└─────────┼──────────────────┼─────────────────────────────┘
          │                  │
          │       WiFiBroadcast Radio Link
          │                  │
┌─────────┼──────────────────┼─────────────────────────────┐
│         │            GROUND UNIT                         │
│         │         Raspberry Pi + WiFi                    │
│         │                  │                             │
│         │           ┌──────┴───────┐   ┌─────────────┐  │
│         │           │   OpenHD     │──►│   QOpenHD    │  │
│         │           │ Ground Proc  │   │  (Qt6 GUI)   │  │
│         │           └──────────────┘   └─────────────┘  │
└──────────────────────────────────────────────────────────┘
```

### Communication Channels (WFB Radio Ports)

| Port | Direction | Content |
|------|-----------|---------|
| 3 | Ground → Air | Telemetry RX (MAVLink uplink) |
| 4 | Air → Ground | Telemetry TX (MAVLink downlink) |
| 10 | Air → Ground | Primary video stream |
| 11 | Air → Ground | Secondary video stream |
| 20 | Air → Ground | Management TX |
| 21 | Ground → Air | Management TX |
| 30 | Air → Ground | Audio stream |
| **40** | **Air → Ground** | **Detection data (Hailo bboxes) — NEW** |

### Local IPC (on Air Unit)

| Endpoint | Protocol | Purpose |
|----------|----------|---------|
| UDP 5510 | JSON | OpenHD → drone-follow (parameter changes) |
| UDP 5511 | JSON | drone-follow → OpenHD (param sync + bboxes) |
| `/tmp/openhd_raw_video` | POSIX SHM | Raw NV12 frames (Mode B only) |
| TCP 5760 | MAVLink | OpenHD → Flight Controller passthrough |

### Local IPC (on Ground Unit)

| Endpoint | Protocol | Purpose |
|----------|----------|---------|
| UDP 5520 | Binary v3 | Detection data → QOpenHD |
| UDP 5600 | RTP H.264 | Primary video → QOpenHD |
| UDP 5601 | RTP H.264 | Secondary video → QOpenHD |

### Component Roles

- **OpenHD Air**: Captures video from camera, encodes, transmits via WFB. Hosts Hailo bridge for parameter sync. Optionally provides raw SHM for drone-follow.
- **OpenHD Ground**: Receives video/telemetry via WFB. Forwards detection data to QOpenHD on UDP 5520.
- **drone-follow**: Runs Hailo AI inference on video frames, tracks persons with ByteTracker, computes velocity commands, sends to flight controller via MAVLink. Reports detections back to OpenHD for overlay.
- **QOpenHD**: Displays video, telemetry HUD, detection overlay (bboxes), and drone-follow control widget. Allows operator to select follow target.

---

## 2. Two Hailo Operating Modes

### Mode A — Camera Type 5 (X_CAM_TYPE_HAILO_AI)

drone-follow **owns the camera** — it captures directly from the RPi camera, runs Hailo inference, draws overlay, encodes to H.264, and streams RTP to OpenHD which treats it as an external video source.

**Command:**
```bash
drone-follow --input rpi --openhd-stream --horizontal-mirror --connection tcpout://127.0.0.1:5760
```

> **Note:** `--horizontal-mirror` is only for **selfie mode** (front-facing camera). Omit for rear-facing.

**Configuration:**
- Set camera type to **5** (HAILO_AI) in QOpenHD camera settings: `sudo vim /boot/openhd/camera1.txt`
- No `hailo.txt` file needed:  `sudo rm /boot/openhd/hailo.txt`

### Mode B — Shared Memory (hailo.txt flag)

OpenHD **owns the camera** — it captures from libcamera as normal, encodes for WFB transmission, and **also** tees raw NV12 frames to a shared memory socket. drone-follow reads from SHM, does AI only, no encoding.

**Command:**
```bash
drone-follow --input shm:///tmp/openhd_raw_video --no-display --connection tcpout://127.0.0.1:5760
```

**Configuration:**
- Set camera type to a normal libcamera type (e.g. **31** = IMX219, **32** = IMX708): `sudo vim /boot/openhd/camera1.txt`
- Create flag file: `sudo touch /boot/openhd/hailo.txt`

### Comparison

| Feature | Mode A (Type 5) | Mode B (SHM) |
|---------|-----------------|---------------|
| Camera ownership | drone-follow | OpenHD |
| Video encoding | drone-follow (x264enc) | OpenHD (v4l2/sw) |
| AI overlay on video | Yes (baked into stream) | No (overlay on QOpenHD only) |
| Resolution control | drone-follow | OpenHD settings |
| Dynamic bitrate | Via DF_BITRATE param | OpenHD native |
| Config needed | Camera type = 5 | Camera type = 30-46 + hailo.txt |
| CPU load (air) | Higher (encode + AI) | Lower (AI only) |

---

## 3. Pipeline Overview

### Mode A: drone-follow Owns Camera

```
                       ═══════════ AIR UNIT ═══════════

                          drone-follow (Python + GStreamer + Hailo)
┌──────────────────────────────────────────────────────────────────────────┐
│                                                                          │
│  libcamerasrc ──► videoscale ──► Hailo Tiling NPU ──► app_callback()    │
│                                                            │             │
│                                                    ┌───────┴────────┐   │
│                                                    │ tee            │   │
│                                                    │  ├─► x264enc   │   │
│                                                    │  │   rtph264pay│   │
│                                                    │  │   udpsink   │──►│──► UDP 5500
│                                                    │  │   :5500     │   │
│                                                    │  └─► fakesink  │   │
│                                                    │      (no disp) │   │
│                                                    └────────────────┘   │
└──────────────────────────────────────────────────────────────────────────┘
                                    │
                              UDP RTP :5500
                                    ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                          OpenHD Air (Camera Type 5)                      │
│                                                                          │
│  udpsrc :5500 ──► rtph264depay ──► h264parse ──► WFB TX (port 10)       │
│                                                                          │
│  HailoFollowBridge ──► binary payload v3 ──► WFB TX (port 40)          │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘

                          ~~~ WiFiBroadcast Radio ~~~

                      ═══════════ GROUND UNIT ═══════════

┌──────────────────────────────────────────────────────────────────────────┐
│                          OpenHD Ground                                   │
│                                                                          │
│  WFB RX (port 10) ──► UDP :5600 ──────────────────────┐                │
│  WFB RX (port 40) ──► detection forwarder ──► UDP :5520 ──┐            │
│                                                        │   │            │
└────────────────────────────────────────────────────────┼───┼────────────┘
                                                         │   │
                                                         ▼   ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                             QOpenHD (Qt6 GUI)                            │
│                                                                          │
│  UDP :5600 ──► H.264 decode ──► Video display                           │
│  UDP :5520 ──► HailoDetectionModel ──► DetectionOverlay (bboxes)        │
│                                    └──► DroneFollowWidget (follow state) │
│                                                                          │
│  MAVLink ◄──► Parameter control (DF_FOLLOW_ID, gains, etc.)            │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### Mode B: OpenHD Owns Camera, Shares Raw via SHM

```
                       ═══════════ AIR UNIT ═══════════

┌──────────────────────────────────────────────────────────────────────────┐
│                     OpenHD Air (libcamera + hailo.txt)                    │
│                                                                          │
│  libcamerasrc ──► capsfilter ──► tee (raw_t)                            │
│                                   ├──► encoder ──► WFB TX (port 10)     │
│                                   └──► queue (leaky) ──► shmsink        │
│                                              /tmp/openhd_raw_video      │
│                                              (10 MB, NV12)              │
│                                                                          │
│  HailoFollowBridge ──► binary payload v3 ──► WFB TX (port 40)          │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
                                    │
                          POSIX SHM socket
                                    ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                          drone-follow (SHM mode)                         │
│                                                                          │
│  shmsrc ──► videoconvert (NV12→I420) ──► videoscale ──► videoconvert    │
│             (writable copy)               ──► Hailo Tiling NPU          │
│                                              ──► app_callback()         │
│                                              ──► fakesink (--no-display)│
│                                                                          │
│  Auto-rebuilds pipeline on SHM disconnection (2s retry)                 │
└──────────────────────────────────────────────────────────────────────────┘

                          ~~~ WiFiBroadcast Radio ~~~

                      ═══════════ GROUND UNIT ═══════════

┌──────────────────────────────────────────────────────────────────────────┐
│                          OpenHD Ground                                   │
│                                                                          │
│  WFB RX (port 10) ──► UDP :5600 ──────────────────────┐                │
│  WFB RX (port 40) ──► detection forwarder ──► UDP :5520 ──┐            │
│                                                        │   │            │
└────────────────────────────────────────────────────────┼───┼────────────┘
                                                         │   │
                                                         ▼   ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                             QOpenHD (Qt6 GUI)                            │
│                                                                          │
│  UDP :5600 ──► H.264 decode ──► Video display                           │
│  UDP :5520 ──► HailoDetectionModel ──► DetectionOverlay (bboxes)        │
│                                    └──► DroneFollowWidget (follow state) │
│                                                                          │
│  MAVLink ◄──► Parameter control (DF_FOLLOW_ID, gains, etc.)            │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### Detection Data Flow (Both Modes)

```
drone-follow                    OpenHD Air                   WFB Radio
  app_callback()                  HailoFollowBridge
  ByteTracker                       │
       │                            │
       ▼                            ▼
  JSON report ──UDP:5511──► on_udp_data()
  {params, bboxes}             parses bboxes
   bboxes: [...]}               builds binary v3
                                    │
                                emit_data_if_cb_set()
                                    │
                                    ▼
                              link_handle->transmit_detection_data()
                                    │
                              WFB TX port 40 ──────────────► WFB RX
                                                                │
                                                          Ground Unit
                                                          OHDVideoGround
                                                          ::on_detection_data()
                                                                │
                                                          UDP :5520
                                                                │
                                                                ▼
                                                          QOpenHD
                                                          HailoDetectionModel
                                                          (parses binary v3)
                                                                │
                                                          ┌─────┴──────┐
                                                          │            │
                                                    DetectionOverlay  DroneFollowWidget
                                                    (bboxes on video) (follow state ring)
```

---

## 4. OpenHD Configuration Reference

### Config Base Paths

| Platform | Base Path |
|----------|-----------|
| RPi (4/5), x86/Ubuntu | `/boot/openhd/` |
| Rockchip, X20 | `/config/openhd/` |

### Role Selection Files & CLI Flags

OpenHD runs as either **air** or **ground** unit. Two ways to set the role:

**CLI flags (preferred for development):**
```bash
sudo openhd --air          # Run as air unit
sudo openhd --ground       # Run as ground unit
```

**File-based (used by OpenHD images):**
- `/boot/openhd/air.txt` — presence means air unit
- `/boot/openhd/ground.txt` — presence means ground unit

### OpenHD CLI Flags Reference

| Flag | Short | Description |
|------|-------|-------------|
| `--air` | `-a` | Run as air unit (creates dummy camera if none found) |
| `--ground` | `-g` | Run as ground unit (no camera detection) |
| `--clean-start` | `-c` | **Wipe all persistent settings**. Recommended on first use or when switching hardware. |
| `--no-qt-autostart` | `-w` | Disable auto-start of QOpenHD on ground |
| `--run-time-seconds N` | `-r N` | Run for N seconds then exit (debugging) |
| `--hardware-config-file PATH` | `-h PATH` | Use custom hardware.config file |

### Persistent Settings (`/usr/local/share/openhd/`)

OpenHD stores all runtime settings and keys in `/usr/local/share/openhd/`. This directory is created on first boot and persists across reboots.

```
/usr/local/share/openhd/
├── unit.id                          # Unique unit identifier (generated once)
├── txrx.key                         # WFB encryption keypair (128 bytes)
├── recording.txt                    # Cached path for video recording location
├── interface/
│   ├── wifibroadcast_settings.json  # WiFi frequency, MCS, bitrate, channel width, FEC
│   └── networking_settings.json     # WiFi hotspot mode, ethernet mode
├── telemetry/
│   ├── air_settings.json            # Air telemetry configuration
│   └── rpi_gpio_control.json        # GPIO control settings (RPi only)
└── video/
    ├── <SENSOR_NAME>_<N>.json       # Per-camera settings (bitrate, AWB, rotation, etc.)
    └── air_camera_generic.json      # Generic air camera settings
```

All modules use the `PersistentSettings<T>` template class which serializes/deserializes settings as JSON. On first boot (or after `--clean-start`) default values are written.

**`--clean-start` deletes this entire directory** and recreates it empty, forcing all modules back to defaults. The WFB encryption key (`txrx.key`) is regenerated after the wipe, so air/ground will still pair (assuming same password).

Other ways to trigger the same reset:
- Place `reset.txt` in the config base path (e.g. `/boot/openhd/reset.txt`) — deleted after reset
- Hold GPIO26 button on RPi during boot

### Key Configuration Files

These files live in the **config base path** (`/boot/openhd/` on RPi/x86, `/config/openhd/` on Rockchip):

| File | Purpose |
|------|---------|
| `hardware.config` | Main configuration file (camera, wifi, general settings) |
| `air.txt` / `ground.txt` | Role selection markers |
| `hailo.txt` | **Presence enables Mode B** (SHM raw passthrough to drone-follow) |
| `reset.txt` | Presence triggers settings reset on next boot (then deleted) |
| `ethernet.txt` | Ethernet link configuration |
| `password.txt` | If present, generates WFB key from this password (then deleted) |

### Camera Type Values

| Type | Constant | Description |
|------|----------|-------------|
| **5** | `X_CAM_TYPE_HAILO_AI` | **Mode A**: External video from drone-follow via UDP RTP |
| 10 | `X_CAM_TYPE_USB_GENERIC` | Generic USB camera |
| 30 | `X_CAM_TYPE_RPI_LIBCAMERA_RPIF_V1_OV5647` | RPi Camera v1 (OV5647) |
| **31** | `X_CAM_TYPE_RPI_LIBCAMERA_RPIF_V2_IMX219` | RPi Camera v2 (IMX219) |
| **32** | `X_CAM_TYPE_RPI_LIBCAMERA_RPIF_V3_IMX708` | RPi Camera v3 (IMX708) |
| 33 | `X_CAM_TYPE_RPI_LIBCAMERA_RPIF_HQ_IMX477` | RPi HQ Camera (IMX477) |
| 40-46 | Various Arducam | Arducam variants |

> For **Mode B**, use any libcamera type (30-46) and create `hailo.txt`.

### WiFi Hotspot

The WiFi hotspot can interfere with operations. Here's how to control it:

**Setting:** `WIFI_HOTSPOT_E` (adjustable via QOpenHD or MAVLink)

| Value | Mode | Behavior |
|-------|------|----------|
| 0 | AUTO | On when disarmed, off when armed |
| **1** | **ALWAYS_OFF** | **Always disabled** (default) |
| 2 | ALWAYS_ON | Always enabled |

**To disable on both air and ground:**

1. **Via QOpenHD:** Set `WIFI_HOTSPOT_E = 1` in Link Settings on each unit (or leave as default).
2. **Via `hardware.config`:** Set the hotspot card to empty so no card is assigned:
   ```ini
   # In /boot/openhd/hardware.config under [wifi]:
   WIFI_WIFI_HOTSPOT_CARD =
   ```
   When this is empty and `WIFI_ENABLE_AUTODETECT = false`, no hotspot is created regardless of the `WIFI_HOTSPOT_E` setting.

**Hotspot details** (when active):
- SSID: `openhd_air` or `openhd_ground`
- Password: `openhdopenhd`
- IP: `192.168.3.1/24`
- Frequency: auto-selected opposite to WFB frequency (5GHz if WFB uses 2.4GHz)
- Config file: `/etc/NetworkManager/system-connections/ohd_wifi_hotspot.nmconnection`

---

## 5. Build & Run Instructions

### 5.1 Building OpenHD (`build_native.sh`)

The unified build script supports RPi4, RPi5, and x86/Ubuntu. It auto-detects the platform.

**Prerequisites — directory layout:**
```
<parent>/
├── OpenHD/              # This repo
├── OpenHD-SysUtils/     # SysUtils repo (cloned separately)
```

**Commands:**
```bash
cd OpenHD/

# Install all dependencies
sudo ./build_native.sh deps

# Build SysUtils + OpenHD, install to /usr/local/bin/openhd
sudo ./build_native.sh build

# Build + install WiFi driver (rtl88x2bu)
sudo ./build_native.sh driver

# All of the above in sequence
sudo ./build_native.sh all
```

**Options:**
- `--enable-service` — Also install and enable systemd services (`openhd.service`)

**What `build` does:**
1. Builds SysUtils (`cmake` + `make`), installs to `/usr/local/bin/openhd_sys_utils`
2. Builds OpenHD (`cmake` + `make`), installs to `/usr/local/bin/openhd`
3. Installs default `hardware.config` to `/boot/openhd/` (only if not already present)

**WiFi driver note:** On RPi, uses the `barakbk-hailo/rtl88x2bu` fork. On x86, uses the `OpenHD/rtl88x2bu` fork.

### 5.2 Running OpenHD

**Air unit:**
```bash
sudo openhd --air

# First run — wipe any stale settings:
sudo openhd --air --clean-start
```

**Ground unit:**
```bash
sudo openhd --ground

# First run:
sudo openhd --ground --clean-start
```

> `--clean-start` is recommended on first use or when switching hardware. It wipes all persistent settings and starts fresh.

**With systemd (if `--enable-service` was used):**
```bash
sudo systemctl start openhd
sudo systemctl stop openhd
sudo systemctl status openhd
```

### 5.3 Building QOpenHD

**Primary method — QMake (`build_qmake.sh`):**
```bash
cd QOpenHD/
./build_qmake.sh
```
This handles translations, creates a `build/` directory, runs `qmake` + `make` with parallel jobs (half CPU cores).

**Alternative — CMake:**
```bash
cd QOpenHD/
./build_cmake.sh    # Requires Qt 6.6.2 at ~/Qt/6.6.2/gcc_64/
```

**Dependencies:** Install via `install_build_dep.sh` or `install_qt6_build_dep.sh`.

### 5.4 Running drone-follow

**Mode A — drone-follow owns camera (OpenHD stream):**
```bash
drone-follow \
    --input rpi \
    --openhd-stream \
    --horizontal-mirror \
    --connection tcpout://127.0.0.1:5760
```

| Flag | Purpose |
|------|---------|
| `--input rpi` | Capture from RPi CSI camera via libcamera |
| `--openhd-stream` | Encode + send RTP to OpenHD on UDP 5500 |
| `--horizontal-mirror` | Mirror video horizontally (**selfie mode only**) |
| `--connection tcpout://127.0.0.1:5760` | MAVLink to flight controller via OpenHD passthrough |

**Mode B — shared memory from OpenHD:**
```bash
drone-follow \
    --input shm:///tmp/openhd_raw_video \
    --no-display \
    --connection tcpout://127.0.0.1:5760
```

| Flag | Purpose |
|------|---------|
| `--input shm:///tmp/openhd_raw_video` | Read raw NV12 from OpenHD's SHM socket |
| `--no-display` | Headless mode (no window, required if no display attached) |
| `--connection tcpout://...` | MAVLink to flight controller |

> **SHM mode note:** Width/height/fps may need to be specified explicitly if auto-detection fails: `--width 1280 --height 720 --fps 30`

**Additional drone-follow flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `--openhd-port` | 5500 | UDP port for RTP output to OpenHD |
| `--openhd-bitrate` | 3917 | H.264 bitrate in kbps |
| `--no-takeoff-landing` | off | Skip auto-arm/takeoff; wait for manual OFFBOARD |
| `--takeoff-altitude` | 3.0 | Takeoff height in meters |
| `--yaw-only` | off | Only rotate, no forward/backward movement |
| `--fixed-altitude` | off | Hold altitude constant |
| `--serial` | — | Connect to FC via USB serial (auto-detects port) |
| `--ui` | off | Enable web UI on port 5001 |

---

## 6. Hailo Bridge Parameters

### Design Decision: Integer-Only MAVLink Transport

OpenHD's MAVLink parameter system **intentionally does not support float parameters** (comments in the codebase confirm this design choice). To work around this, the Hailo bridge uses **integer scaling**:

- Float values are multiplied by 100 before storing as MAVLink INT32
- Example: `kp_yaw = 5.0` is exposed as `DF_KP_YAW = 500`
- The bridge converts transparently — the Python app always sends/receives unscaled floats
- This gives **2 decimal places** of precision

### Parameter Table

All parameters are registered as MAVLink INT32 and appear in QOpenHD settings.

| MAVLink ID | Python Name | Type | Default | Description |
|------------|-------------|------|---------|-------------|
| `DF_KP_YAW` | `kp_yaw` | Float (×100) | 5.0 (500) | Yaw proportional gain |
| `DF_KP_FWD` | `kp_forward` | Float (×100) | 3.0 (300) | Forward proportional gain |
| `DF_KP_BACK` | `kp_backward` | Float (×100) | 5.0 (500) | Backward proportional gain |
| `DF_MAX_FWD` | `max_forward` | Float (×100) | 2.0 (200) | Max forward speed (m/s) |
| `DF_MAX_BACK` | `max_backward` | Float (×100) | 3.0 (300) | Max backward speed (m/s) |
| `DF_TGT_DIST` | `target_distance_m` | Float (×100) | 0.0 (0) | Target distance in meters (0=disabled) |
| `DF_DZ_H_PCT` | `dead_zone_height_percent` | Float (×100) | 5.0 (500) | Forward dead zone (% of target bbox height) |
| `DF_YAW_ALPHA` | `yaw_alpha` | Float (×100) | 0.3 (30) | Yaw EMA smoothing factor |
| `DF_FWD_ALPHA` | `forward_alpha` | Float (×100) | 0.1 (10) | Forward EMA smoothing factor |
| `DF_TAKEOFF_M` | `takeoff_altitude` | Float (×100) | 3.0 (300) | Takeoff altitude in meters |
| `DF_YAW_ONLY` | `yaw_only` | Int | 0 | Only rotate, no fwd/back (0/1) |
| `DF_FIX_ALT` | `fixed_altitude` | Int | 0 | Hold altitude constant (0/1) |
| `DF_SMTH_YAW` | `smooth_yaw` | Int | 1 | Enable yaw EMA smoothing (0/1) |
| `DF_SMTH_FWD` | `smooth_forward` | Int | 1 | Enable forward EMA smoothing (0/1) |
| `DF_FOLLOW_ID` | `follow_id` | Int | 0 | Follow target: -1=idle, 0=auto, N=lock to ID N |
| `DF_ACTIVE_ID` | `active_id` | Int | 0 | **Read-only.** Currently tracked person ID (0=none) |
| `DF_BITRATE` | `bitrate_kbps` | Int | 3917 | Encoder bitrate (kbps), auto-updated by WFB link |

### JSON Wire Protocol

**OpenHD → Python (UDP 5510):**
```json
{"param": "follow_id", "value": -1}
{"param": "kp_yaw", "value": 5.5}
{"param": "bitrate_kbps", "value": 4000}
```

**Python → OpenHD (UDP 5511, periodic ~10Hz):**
```json
{
  "params": {
    "kp_yaw": 5.0,
    "kp_forward": 3.0,
    "follow_id": 0,
    "active_id": 3,
    ...
  },
  "bboxes": [
    {"id": 1, "cx": 0.45, "cy": 0.5, "w": 0.1, "h": 0.2, "tracked": false},
    {"id": 3, "cx": 0.55, "cy": 0.48, "w": 0.12, "h": 0.25, "tracked": true}
  ]
}
```

### Follow ID Semantics

| Value | Mode | Behavior |
|-------|------|----------|
| -1 | IDLE | Drone holds position, ignores all detections |
| 0 | AUTO | Follow the largest person in frame |
| N (>0) | LOCKED | Follow person with tracking ID N; falls back to IDLE if lost |

---

## 7. Binary Detection Payload v3

Transmitted via dedicated WFB stream (port 40) from air to ground, then forwarded to QOpenHD on UDP 5520.

### Header (6 bytes)

| Offset | Size | Type | Field |
|--------|------|------|-------|
| 0 | 1 | uint8 | `version` = 3 |
| 1-2 | 2 | uint16 LE | `active_id` (0 = no one tracked) |
| 3-4 | 2 | int16 LE | `follow_id` (-1=idle, 0=auto, N=locked) |
| 5 | 1 | uint8 | `count` (number of bboxes, max 126) |

### Per Bounding Box (11 bytes each)

| Offset | Size | Type | Field |
|--------|------|------|-------|
| 0-1 | 2 | uint16 LE | `id` (tracking ID) |
| 2-3 | 2 | uint16 LE | `cx` (center X, normalized) |
| 4-5 | 2 | uint16 LE | `cy` (center Y, normalized) |
| 6-7 | 2 | uint16 LE | `w` (width, normalized) |
| 8-9 | 2 | uint16 LE | `h` (height, normalized) |
| 10 | 1 | uint8 | `flags` (bit 0 = is_tracked) |

**Coordinate normalization:** `0` = 0.0, `65535` = 1.0 (mapping: `uint16 = float * 65535 + 0.5`)

**Total packet size:** `6 + count × 11` bytes (max ~1392 bytes within MTU)

---

## 8. QOpenHD Detection Display

### HailoDetectionModel

**File:** `QOpenHD/app/telemetry/models/hailodetectionmodel.{h,cpp}`

- Singleton listening on **UDP 5520** for binary detection payloads
- Parses binary v3 payloads
- Exposes to QML as `_hailoDetectionModel`:
  - `active_id` — currently tracked person ID
  - `follow_id` — operator's follow selection
  - `detections` — QVariantList of `{id, cx, cy, w, h, tracked}`
  - `receiving` — liveness flag (true if data received within last 2 seconds)

### DetectionOverlay

**File:** `QOpenHD/qml/ui/DetectionOverlay.qml`

- Full-screen transparent overlay drawn on top of the video
- Adjusts for letterboxing (reads actual stream resolution from `_decodingStatistics`)
- Bounding box colors:
  - **Green (thick border)** — tracked person (the one being followed)
  - **White (thin border)** — detected but not tracked
- Shows tracking ID label above each box
- **Auto-hides** when `_hailoDetectionModel.receiving` is false (2s timeout)

### DroneFollowWidget

**File:** `QOpenHD/qml/ui/widgets/DroneFollowWidget.qml`

- Centered on the HUD horizon indicator
- Color-coded ring + status label:
  - **Red `#N`** — Operator locked to person N
  - **Amber `IDLE`** — Drone holding position
  - **Teal `AUTO #N`** — System auto-selected person N
  - **Gray `AUTO`** — Auto mode, no detections yet
- Tap to open control popup:
  - List of visible person IDs to lock onto
  - AUTO button (follow largest)
  - IDLE button (hold position)
- Changes `DF_FOLLOW_ID` via MAVLink parameter write
- **Auto-hides** when detection data stream stops

---

## 9. Key Technical Details

### CPU Affinity (OpenHD Air)

On systems with 4+ CPU cores, OpenHD pins itself to CPUs 1-N, reserving CPU#0 for IRQ handling. Without this, heavy video pipelines (software encode, videoscale) can saturate CPU#0 and freeze keyboard/mouse/network I/O.

**Source:** `OpenHD/main.cpp:189-206`

### Full FOV Mode (libcamerasrc)

When the target resolution fits within the camera's full field-of-view, OpenHD uses the sensor's binned resolution to maximize FOV:

| Sensor | Full FOV Resolution | Native |
|--------|-------------------|--------|
| OV5647 | 1296×972 | 2592×1944 binned 2×2 |
| IMX219 | 1640×1232 | 3280×2464 binned 2×2 |
| IMX708 | 2304×1296 | 4608×2592 binned 2×2 |
| IMX477 | 2028×1520 | 4056×3040 binned 2×2 |

Pipeline: `libcamerasrc (full FOV) → videocrop (aspect ratio) → videoscale (target resolution)`

**Source:** `OpenHD/ohd_video/inc/gst_helper.hpp` — `getFullFovIspResolution()`

### Stock libcamerasrc Properties

Migrated from custom OpenHD properties to **stock kebab-case** properties for compatibility with upstream libcamerasrc builds:

| Old (custom) | New (stock) |
|-------------|-------------|
| `ev=` | `exposure-value=` |
| `awb=` | `awb-mode=` |
| `metering=` | `ae-metering-mode=` |
| `exposure=` | `ae-exposure-mode=` |
| `shutter=` | `exposure-time=` |
| `hflip=`, `vflip=`, `rotation=` | Removed |
| `denoise=` | Removed (unsupported on stock) |

### Resolution Auto-Detection

For external video sources (HAILO_AI, EXTERNAL, EXTERNAL_IP), OpenHD monitors the h264/h265 parser output caps after the stream starts flowing, extracts the actual width/height/fps, and updates the LinkActionHandler so QOpenHD can display the correct resolution.

**Source:** `OpenHD/ohd_video/src/gstreamerstream.cpp` — `try_detect_stream_resolution()`

### Dynamic Bitrate Control

When WFB link quality changes, the recommended bitrate is propagated:
1. WFB link calculates target bitrate based on TX errors
2. `LinkActionHandler::action_request_bitrate_change_handle()` is called
3. For **Hailo mode**: `HailoFollowBridge::update_param("bitrate_kbps", value)` sends to drone-follow
4. For **normal cameras**: Encoder bitrate property is set directly

### SHM Auto-Rebuild (drone-follow)

In SHM mode, if the shared memory source disconnects (e.g. OpenHD restarts), drone-follow automatically rebuilds its entire GStreamer pipeline after a 2-second delay.

### ByteTracker (drone-follow)

Multi-person tracking using Kalman filter + Hungarian algorithm:
- Track activation: requires 3 consecutive detections
- Track buffer: keeps tracks alive for 90 frames (~3s at 30fps)
- Match threshold: 0.5 IoU for association
- Confidence threshold: 0.4 for tracking

### Forward Velocity Smoother

Optional EMA (Exponential Moving Average) smoothing on forward velocity:
- Estimates person approach rate via `d(bbox_height)/dt`
- Derivative feed-forward for proactive speed adjustment
- `forward_alpha` controls smoothing (0.1 = very smooth, 1.0 = no smoothing)

---

## 10. High-Level Changelog (Final State)

Summary of what was added/changed across all three repositories (feature perspective, not commit-by-commit).

### OpenHD

| Area | What Changed |
|------|-------------|
| **Hailo Follow Bridge** | New module (`hailo_follow_bridge.{h,cpp}`) — registers 17 DF_* MAVLink parameters, bidirectional UDP JSON bridge to drone-follow app (ports 5510/5511), builds binary detection payload v3, emits via WFB port 40. |
| **Detection Data Stream** | New dedicated WFB radio port 40 (air→ground, no FEC) for real-time bounding box data. Ground unit forwards to QOpenHD on UDP 5520. |
| **SHM Raw Passthrough** | When `/boot/openhd/hailo.txt` exists + libcamera camera, tees raw NV12 frames to `/tmp/openhd_raw_video` (10MB shared memory) for drone-follow's AI pipeline. |
| **Camera Type 5** | New `X_CAM_TYPE_HAILO_AI` — receives AI-processed video from drone-follow via UDP RTP. Resolution auto-detected from stream caps (not user-configurable). |
| **Dynamic Bitrate** | WFB link bitrate recommendations forwarded to drone-follow's encoder via `DF_BITRATE` parameter. |
| **Full FOV Mode** | Auto-detects sensor binned resolution (OV5647, IMX219, IMX708, IMX477) to maximize field-of-view when target resolution allows it. |
| **Stock libcamerasrc** | Migrated from custom properties to upstream kebab-case properties (`exposure-value`, `awb-mode`, etc.) for compatibility with stock builds. |
| **CPU Affinity** | Pins OpenHD to CPUs 1-N on 4+ core systems, reserving CPU#0 for IRQ handling to prevent I/O freezes. |
| **Build System** | Unified `build_native.sh` supporting RPi4, RPi5, and x86/Ubuntu with auto-detection, dependency management, WiFi driver build, and optional systemd integration. |
| **Defaults** | WiFi hotspot default changed to off. `hardware.config` only installed if not already present. RPi5 default MCS set to 0. `raspi-gpio` → `pinctrl` migration. |

### QOpenHD

| Area | What Changed |
|------|-------------|
| **HailoDetectionModel** | New model (`hailodetectionmodel.{h,cpp}`) — UDP listener on port 5520, parses binary detection payload v3, exposes `active_id`, `follow_id`, `detections`, `receiving` to QML. |
| **DetectionOverlay** | New QML overlay — renders bounding boxes on video (green=tracked, white=detected), auto-adjusts for letterboxing, auto-hides when detection stream stops (2s timeout). |
| **DroneFollowWidget** | New QML widget — color-coded follow state ring (Red=locked, Amber=idle, Teal=auto), tap-to-control popup with target selection, AUTO, and IDLE buttons. |
| **Camera Type 5** | Added `X_CAM_TYPE_HAILO_AI` to camera type definitions. Fixed RPi5 platform type collision (now 13). |
| **Live Detection Binding** | Changed from MAVLink param polling (1Hz, stale) to direct binary payload parsing (10Hz, real-time) for follow state display. |
| **VARIABLE_BITRATE** | Unhidden in Link Settings UI so users can toggle dynamic bitrate. |

### hailo-drone-follow

| Area | What Changed |
|------|-------------|
| **OpenHD Stream (Mode A)** | `--openhd-stream` flag — encodes overlay video to H.264 RTP, sends to OpenHD on UDP 5500. `--horizontal-mirror` for selfie mode. |
| **SHM Input (Mode B)** | `--input shm:///path` — reads raw NV12 from OpenHD shared memory. Auto-rebuilds pipeline on disconnection (2s retry). |
| **OpenHD Bridge** | Bidirectional UDP parameter sync (ports 5510/5511). Reports detections + bboxes as JSON. Receives parameter changes and dynamic bitrate from OpenHD. |
| **Follow State Machine** | IDLE/AUTO/LOCKED states with `follow_id` semantics (-1/0/N). Falls back to IDLE when explicitly locked target is lost. |
| **Dynamic Bitrate** | Applies WFB bitrate recommendations to x264enc encoder at runtime. Default aligned to 3917 kbps. |
| **Headless Mode** | `--no-display` for running without a display attached. |

---

## 11. Detailed Changelog — OpenHD

Branch: `feature/hailo-apps-integration` (based on `2.6-evo-hailo`)
**35 files changed, +1096 / -215 lines**

| Commit | Description |
|--------|-------------|
| `55a4feaf` | **Add Hailo integration docs, remove DF_AVAIL_IDS** — Added HAILO_INTEGRATION.md, linked from README. Removed unused `DF_AVAIL_IDS` MAVLink parameter and `avail_ids` JSON parsing from HailoFollowBridge. |
| `5f832004` | **Hide resolution setting for Hailo AI** — Resolution is determined by the drone pipeline, not configurable from OpenHD. Added auto-detection of actual stream resolution from parser caps. |
| `424e9511` | **Dynamic bitrate control + RPi5 MCS0 default** — Forward WFB bitrate recommendations to drone-follow app via HailoFollowBridge. Set RPi5 default MCS to 0. |
| `3f4a8347` | **Add RPi4 support to build_native.sh** — Platform detection and build paths for RPi4. |
| `e7b12375` | **Add follow_id to binary detection payload v3** — Payload now includes operator's follow intent (-1/0/N) alongside active_id, enabling QOpenHD to display follow state without polling MAVLink. |
| `176bfc4d` | **Fix pipeline crashes, stock libcamerasrc properties, RPi5 stability** — Migrated to stock kebab-case properties. Added full FOV mode. Fixed pipeline startup failures. |
| `dd2b844e` | **Fixed stale SHM socket + full FOV options** — Remove stale `/tmp/openhd_raw_video` on startup. Added sensor-aware full FOV resolution selection. |
| `cb309497` | **Renamed build_rpi5_native to build_native** — Generalized the build script for all platforms. |
| `6335df95` | **Fixes and generalizations for build script** — Error handling, dependency fixes. |
| `af0de19d` | **Added SHM option** — Raw NV12 shared memory passthrough via `shmsink` in libcamera pipeline when `hailo.txt` exists. |
| `a3caa7a2` | **Working overlay integration — custom WFB port** — Detection data transmitted on dedicated WFB port 40 instead of tunneling through telemetry. |
| `4e65a792` | **Working overlay integration — limited to 11 bboxes** — Binary detection payload with size limits for reliable transmission. |
| `021947d2` | **Added state machine for OpenHD UI** — Detection overlay state management in QOpenHD. |
| `b5ed2104` | **Added int scaling hack for float params** — ×100 scaling for float parameters in MAVLink INT32 transport. |
| `aa74425a` | **Support float parameters with Hailo bridge** — HailoFollowBridge parameter registration with float→int conversion. |
| `de1d31a8` | **Initial hailo-apps integration** — HailoFollowBridge, UDP parameter sync, detection data callback wiring in main.cpp. |
| `93ac1489` | **Changed default build location** — build_rpi5_native script path update. |
| `33c783c0` | **Default hardware.config + WiFi hotspot off** — Install default config only if not present. Changed WiFi hotspot default to off. |
| `bf1b1f8d` | **Changed raspi-gpio to pinctrl** — RPi5 GPIO tool migration. |

---

## 12. Detailed Changelog — QOpenHD

Branch: `feature/drone-follow-control`

| Commit | Description |
|--------|-------------|
| `b4bcdd7e1` | **Remove v1/v2 detection payload compat** — Simplified `on_udp_data()` to v3-only parsing. Removed v1 (uint8 IDs, 10-byte entries) and v2 (4-byte header) compat branches. |
| `7dc2c1fdb` | **Unhide VARIABLE_BITRATE setting** — Visible in Link Settings so users can toggle dynamic bitrate. |
| `4335a7b2f` | **Bind follow state to live detection stream** — Changed from MAVLink param polling (1Hz) to real-time binary payload parsing (10Hz) for follow state display. |
| `6401568fe` | **Show Hailo widgets only when active** — DroneFollowWidget and DetectionOverlay auto-hide with 2-second liveness timeout. |
| `845e6ab69` | **Hailo AI camera type + RPi5 fix** — Added X_CAM_TYPE_HAILO_AI (type 5). Fixed RPi5 platform type collision (now 13 vs old 12). Enabled CSI camera selection UI. |
| `a8aa511cd` | **Working overlay integration — custom WFB port** — Detection overlay receives data from dedicated WFB port 40. |
| `97bbe5324` | **Working overlay integration — limited to 11 bboxes** — DetectionOverlay renders up to 11 bounding boxes per frame. |
| `3c780d0c0` | **UI redesign** — DroneFollowWidget visual overhaul. |
| `75177f834` | **UI/UX redesign** — Improved DroneFollowWidget layout and interactions. |
| `cc791ded2` | **Optimistic reset** — UI state management improvements. |
| `2db2884e2` | **Updated DroneFollowWidget** — Better follow state display and target selection UX. |
| `338eb4362` | **Additional drone follow changes** — Widget refinements. |
| `0cabc49da` | **Added parameters for drone follow** — Initial DF_* parameter display in settings. |

---

## 13. Detailed Changelog — hailo-drone-follow

Branch: `feature/openhd-integration-new`

| Commit | Description |
|--------|-------------|
| `c021162` | **Remove avail_ids from JSON report** — Removed `avail_ids` from Python→OpenHD JSON report (QOpenHD computes from binary payload). Updated docstring to clarify JSON carries only config params. |
| `5abd37d` | **Dynamic bitrate from OpenHD** — OpenHD bridge applies WFB bitrate recommendations to x264enc encoder at runtime. Aligned default to 3917 kbps. |
| `b3ab95b` | **Fall back to IDLE on lost lock** — When explicitly locked target leaves frame, falls back to IDLE instead of auto-selecting a new target. |
| `dd2aaa3` | **Auto-rebuild SHM pipeline** — Automatically rebuilds GStreamer pipeline on SHM source disconnection (2s retry). |
| `a3ff1dc` | **Use shm:// URI format** — Changed SHM input from `--shm` flag to `--input shm:///path` URI syntax. |
| `0c463cf` | **Added --no-display option** — Headless mode for running without a display. |
| `3de2a68` | **Fixed setup_env** — Don't assume `/home/hailo` path, use relative paths. |
| `44db104` | **Added SHM option** — Initial shared memory integration for OpenHD raw video passthrough. |
| `d13f8c0` | **Move OPENHD_STREAM_PIPELINE to local helper** — Code organization for OpenHD stream pipeline construction. |
| `f320fb2` | **Working overlay integration — custom WFB port** — Detection data sent via OpenHD bridge for WFB port 40 transmission. |
| `efa2c0d` | **Working overlay integration — limited to 11 bboxes** — Limit bbox count per report for payload size safety. |
| `6e3052e` | **Added state machine for OpenHD UI** — Follow state machine (IDLE/AUTO/LOCKED) for QOpenHD integration. |
| `410bb52` | **Support float parameters** — OpenHD bridge handles float↔int scaling for PID gain parameters. |
| `eccf31c` | **Initial support for OpenHD stream** — H.264 RTP encoding + UDP output for Mode A integration. |
| `1323dd6` | **Fix offboard mode resilience** — Robust offboard start/restart and recording pipeline isolation. |

---

## Appendix: Quick Reference Commands

### First-Time Setup (RPi5 Air Unit)

```bash
# 1. Build OpenHD
cd OpenHD/
sudo ./build_native.sh all

# 2. Configure as air with Hailo
sudo touch /boot/openhd/hailo.txt    # Only for Mode B

# 3. Start OpenHD
sudo openhd --air --clean-start

# 4. Install drone-follow
cd hailo-drone-follow/
source setup_env.sh
./install.sh
pip install -e .

# 5. Start drone-follow (Mode A)
drone-follow --input rpi --openhd-stream --connection tcpout://127.0.0.1:5760

# 5. OR Start drone-follow (Mode B)
drone-follow --input shm:///tmp/openhd_raw_video --no-display --connection tcpout://127.0.0.1:5760
```

### Ground Unit Setup

```bash
# 1. Build OpenHD
cd OpenHD/
sudo ./build_native.sh all

# 2. Start OpenHD
sudo openhd --ground --clean-start

# 3. Build + Run QOpenHD
cd QOpenHD/
./build_qmake.sh
cd build/ && ./QOpenHD
```

### Disable WiFi Hotspot (both units)

Set `WIFI_HOTSPOT_E = 1` in QOpenHD Link Settings (default is already off), or set `WIFI_WIFI_HOTSPOT_CARD =` (empty) in `/boot/openhd/hardware.config`.
