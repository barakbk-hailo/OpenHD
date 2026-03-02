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

#include "hailo_follow_bridge.h"

#include "include_json.hpp"
#include "openhd_spdlog_include.h"
#include "openhd_udp.h"

using PT = HailoFollowBridge::ParamType;

static const std::vector<HailoFollowBridge::ParamDef> PARAM_DEFS = {
    // Controller config params (float — native MAVLink REAL32)
    {"DF_KP_YAW", "kp_yaw", PT::FLOAT, 5.0f},
    {"DF_KP_FWD", "kp_forward", PT::FLOAT, 3.0f},
    {"DF_KP_BACK", "kp_backward", PT::FLOAT, 5.0f},
    {"DF_MAX_FWD", "max_forward", PT::FLOAT, 2.0f},
    {"DF_MAX_BACK", "max_backward", PT::FLOAT, 3.0f},
    {"DF_TGT_DIST", "target_distance_m", PT::FLOAT, 0.0f},  // 0 = disabled
    {"DF_DZ_H_PCT", "dead_zone_height_percent", PT::FLOAT, 5.0f},
    {"DF_YAW_ALPHA", "yaw_alpha", PT::FLOAT, 0.3f},
    {"DF_FWD_ALPHA", "forward_alpha", PT::FLOAT, 0.1f},
    {"DF_TAKEOFF_M", "takeoff_altitude", PT::FLOAT, 3.0f},
    // Controller config params (int/bool — MAVLink INT32)
    {"DF_YAW_ONLY", "yaw_only", PT::INT, 0},
    {"DF_FIX_ALT", "fixed_altitude", PT::INT, 0},
    {"DF_SMTH_YAW", "smooth_yaw", PT::INT, 1},
    {"DF_SMTH_FWD", "smooth_forward", PT::INT, 1},
    // Follow target control (from follow_server)
    // DF_FOLLOW_ID: set to a tracking ID to follow, 0 = follow largest, -1 =
    // clear
    {"DF_FOLLOW_ID", "follow_id", PT::INT, 0},
};

const std::vector<HailoFollowBridge::ParamDef>&
HailoFollowBridge::get_param_defs() {
  return PARAM_DEFS;
}

HailoFollowBridge::HailoFollowBridge() {
  m_console = openhd::log::create_or_get("hailo_bridge");
  m_console->info("HailoFollowBridge starting");

  // Initialize parameter cache with defaults
  for (const auto& def : get_param_defs()) {
    m_params[def.python_name] = def.default_value;
  }

  // UDP sender: forwards parameter changes to the Python app
  m_udp_sender = std::make_unique<openhd::UDPForwarder>(
      openhd::ADDRESS_LOCALHOST, SEND_PORT);

  // UDP receiver: listens for value reports from the Python app
  m_udp_receiver = std::make_unique<openhd::UDPReceiver>(
      openhd::ADDRESS_LOCALHOST, LISTEN_PORT,
      [this](const uint8_t* data, std::size_t len) {
        on_udp_data(data, len);
      });
  m_udp_receiver->runInBackground();

  m_console->info("HailoFollowBridge ready (send={}, listen={})", SEND_PORT,
                  LISTEN_PORT);
}

HailoFollowBridge::~HailoFollowBridge() {
  if (m_udp_receiver) {
    m_udp_receiver->stopBackground();
  }
}

float HailoFollowBridge::get_param(const std::string& python_name) const {
  std::lock_guard<std::mutex> lock(m_params_mutex);
  auto it = m_params.find(python_name);
  if (it != m_params.end()) return it->second;
  return 0.0f;
}

void HailoFollowBridge::set_param(const std::string& python_name, float value) {
  std::lock_guard<std::mutex> lock(m_params_mutex);
  m_params[python_name] = value;
}

void HailoFollowBridge::send_param_to_python(const std::string& python_name,
                                              float value) {
  try {
    nlohmann::json j;
    j["param"] = python_name;
    j["value"] = value;
    const std::string msg = j.dump();
    m_udp_sender->forwardPacketViaUDP(
        reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    m_console->debug("Sent to Python: {}", msg);
  } catch (const std::exception& e) {
    m_console->warn("Failed to send param to Python: {}", e.what());
  }
}

void HailoFollowBridge::on_udp_data(const uint8_t* data, std::size_t len) {
  try {
    std::string raw(reinterpret_cast<const char*>(data), len);
    auto j = nlohmann::json::parse(raw);
    if (!j.contains("params") || !j["params"].is_object()) return;
    const auto& params = j["params"];
    std::lock_guard<std::mutex> lock(m_params_mutex);
    for (auto& [key, val] : params.items()) {
      if (val.is_number() && m_params.count(key)) {
        m_params[key] = val.get<float>();
      }
    }
    m_console->debug("Received sync from Python ({} params)", params.size());
  } catch (const std::exception& e) {
    m_console->warn("Failed to parse Python report: {}", e.what());
  }
}

std::vector<openhd::Setting> HailoFollowBridge::get_all_settings() {
  std::vector<openhd::Setting> ret;
  for (const auto& def : get_param_defs()) {
    if (def.type == ParamType::FLOAT) {
      auto change_cb = [this, name = def.python_name](std::string,
                                                       float value) -> bool {
        set_param(name, value);
        send_param_to_python(name, value);
        return true;
      };
      auto get_cb = [this, name = def.python_name]() -> float {
        return get_param(name);
      };
      ret.push_back(openhd::Setting{
          def.mavlink_id,
          openhd::FloatSetting{def.default_value, change_cb, get_cb}});
    } else {
      auto change_cb = [this, name = def.python_name](std::string,
                                                       int value) -> bool {
        set_param(name, static_cast<float>(value));
        send_param_to_python(name, static_cast<float>(value));
        return true;
      };
      auto get_cb = [this, name = def.python_name]() -> int {
        return static_cast<int>(get_param(name));
      };
      ret.push_back(openhd::Setting{
          def.mavlink_id,
          openhd::IntSetting{static_cast<int>(def.default_value), change_cb,
                             get_cb}});
    }
  }
  return ret;
}
