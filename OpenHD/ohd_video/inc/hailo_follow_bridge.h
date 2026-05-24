/******************************************************************************
 * OpenHD
 *
 * Licensed under the GNU General Public License (GPL) Version 3.
 *
 * This software is provided "as-is," without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability,
 * fitness for a particular purpose, and non-infringement. For details, see the
 * full license in the LICENSE file provided with this source code.
 *
 * Non-Military Use Only:
 * This software and its associated components are explicitly intended for
 * civilian and non-military purposes. Use in any military or defense
 * applications is strictly prohibited unless explicitly and individually
 * licensed otherwise by the OpenHD Team.
 *
 * Contributors:
 * A full list of contributors can be found at the OpenHD GitHub repository:
 * https://github.com/OpenHD
 *
 * © OpenHD, All Rights Reserved.
 ******************************************************************************/

#ifndef OPENHD_HAILO_FOLLOW_BRIDGE_H
#define OPENHD_HAILO_FOLLOW_BRIDGE_H

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "openhd_settings_imp.h"
#include "openhd_spdlog.h"

namespace openhd {
class UDPForwarder;
class UDPReceiver;
}  // namespace openhd

// Bridge between OpenHD MAVLink parameter system and the Hailo drone follow
// Python app. Registers DF_* parameters as MAVLink settings so they appear in
// QOpenHD. Parameter changes are forwarded to the Python app via UDP JSON on
// localhost, and current values are received back for read-back sync.
//
// All params are registered as MAVLink INT32.
// FLOAT-typed params (e.g. PID gains) are scaled ×100 in MAVLink
// (e.g. kp_yaw=5.0 is exposed as DF_KP_YAW=500). The bridge converts
// internally; the Python app always receives/sends unscaled float values.
//
// Wire protocol (JSON over UDP):
//   OpenHD -> Python (port 5510): {"param":"<name>","value":<number>}
//   Python -> OpenHD (port 5511): {"params":{"<name>":<number>,...},
//                                   "bboxes": [{"id":.., "cx":.., "cy":..,
//                                               "w":.., "h":.., "tracked":..}]}
//
// Binary payload format v2 (dedicated wfb stream, no size limit):
//   Byte 0:      version = 2
//   Byte 1-2:    active_id (uint16 LE, 0=none)
//   Byte 3:      count (uint8)
//   Per bbox (11 bytes):
//     [0-1] id      uint16 LE
//     [2-3] cx      uint16 LE (0-65535 normalized, 0=0.0, 65535=1.0)
//     [4-5] cy      uint16 LE
//     [6-7] w       uint16 LE
//     [8-9] h       uint16 LE
//     [10]  flags   uint8 (bit0 = is_tracked)

class HailoFollowBridge {
 public:
  HailoFollowBridge();
  ~HailoFollowBridge();

  std::vector<openhd::Setting> get_all_settings();

  // Callback invoked with binary detection payload bytes whenever new bbox data
  // arrives from Python. Transmitted via dedicated wfb stream to ground.
  using DataCb = std::function<void(std::vector<uint8_t>)>;
  void set_data_cb(DataCb cb);

  // Update a parameter and immediately forward it to the Python app via UDP.
  // Used by OHDVideoAir to push WFB bitrate recommendations.
  void update_param(const std::string& python_name, float value);

  static constexpr int SEND_PORT = 5510;    // OpenHD -> Python
  static constexpr int LISTEN_PORT = 5511;  // Python -> OpenHD

  enum class ParamType { INT, FLOAT };

  struct ParamDef {
    std::string mavlink_id;
    std::string python_name;
    ParamType type;
    float default_value;  // used for both int and float (cast as needed)
  };

 private:
  std::shared_ptr<spdlog::logger> m_console;
  static const std::vector<ParamDef>& get_param_defs();

  // Thread-safe parameter cache (stores all values as float for simplicity)
  mutable std::mutex m_params_mutex;
  std::map<std::string, float> m_params;  // keyed by python_name

  // Pending bbox data for TUNNEL emission (updated by on_udp_data)
  struct BboxEntry {
    uint16_t id;
    float cx, cy, w, h;
    bool tracked;
  };
  std::vector<BboxEntry> m_pending_bboxes;
  uint16_t m_pending_active_id = 0;
  // Follow mode from the Python report — emitted in the v4 binary payload's
  // byte-5 slot. 0=AUTO, 1=LOCKED, 2=SEARCH, 3=IDLE. Older Python that
  // doesn't send "mode" leaves this at 0; the wire byte then conveys AUTO,
  // which is what v3-era ground stations had to assume anyway.
  uint8_t m_pending_mode = 0;

  // Callback to emit detection data payloads via wfb stream
  DataCb m_data_cb;

  // Build binary payload from pending bboxes and call m_data_cb
  void emit_data_if_cb_set();

  float get_param(const std::string& python_name) const;
  void set_param(const std::string& python_name, float value);

  // Send a single parameter change to the Python app via UDP JSON
  void send_param_to_python(const std::string& python_name, float value);
  std::unique_ptr<openhd::UDPForwarder> m_udp_sender;

  // Listen for value reports from the Python app
  std::unique_ptr<openhd::UDPReceiver> m_udp_receiver;
  void on_udp_data(const uint8_t* data, std::size_t len);
};

#endif  // OPENHD_HAILO_FOLLOW_BRIDGE_H
