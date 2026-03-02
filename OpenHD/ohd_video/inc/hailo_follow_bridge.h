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
// Float params use native MAVLink REAL32 (no scaling needed).
// Int/bool params use MAVLink INT32.
//
// Wire protocol (JSON over UDP):
//   OpenHD -> Python (port 5510): {"param":"<name>","value":<number>}
//   Python -> OpenHD (port 5511): {"params":{"<name>":<number>,...}}
class HailoFollowBridge {
 public:
  HailoFollowBridge();
  ~HailoFollowBridge();

  std::vector<openhd::Setting> get_all_settings();

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
