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
    // Controller config params (FLOAT — scaled ×100 in MAVLink, e.g. 5.0 → 500)
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
    // Controller config params (INT — value as-is in MAVLink)
    {"DF_YAW_ONLY", "yaw_only", PT::INT, 0},
    {"DF_FIX_ALT", "fixed_altitude", PT::INT, 0},
    {"DF_SMTH_YAW", "smooth_yaw", PT::INT, 1},
    {"DF_SMTH_FWD", "smooth_forward", PT::INT, 1},
    // Follow target control (from follow_server)
    // DF_FOLLOW_ID: set to a tracking ID to follow, 0 = follow largest, -1 = idle
    {"DF_FOLLOW_ID", "follow_id", PT::INT, 0},
    // DF_ACTIVE_ID: read-only — the ID currently being tracked by the Python app
    // (auto-selected or operator-locked). 0 = no one in view.
    // QOpenHD uses this alongside DF_FOLLOW_ID to show "AUTO · #N" in the badge.
    {"DF_ACTIVE_ID", "active_id", PT::INT, 0},
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

void HailoFollowBridge::set_data_cb(DataCb cb) {
  std::lock_guard<std::mutex> lock(m_params_mutex);
  m_data_cb = std::move(cb);
}

void HailoFollowBridge::emit_data_if_cb_set() {
  // m_params_mutex already held by caller
  if (!m_data_cb) return;

  // Binary payload format v3:
  //   [0]     version = 3
  //   [1-2]   active_id (uint16 LE)
  //   [3-4]   follow_id (int16 LE, -1=idle, 0=auto, N=locked)
  //   [5]     count (uint8)
  //   Per bbox (11 bytes): id(2) cx(2) cy(2) w(2) h(2) flags(1)
  // Max ~126 bboxes per packet (1400 byte MTU safety margin)
  const uint8_t count = static_cast<uint8_t>(
      std::min(m_pending_bboxes.size(), static_cast<size_t>(126)));
  // follow_id from params cache (set by Python report or QOpenHD)
  auto fi_it = m_params.find("follow_id");
  const int16_t follow_id = fi_it != m_params.end()
      ? static_cast<int16_t>(fi_it->second) : 0;
  std::vector<uint8_t> payload;
  payload.reserve(6 + count * 11);
  payload.push_back(3);  // version
  payload.push_back(static_cast<uint8_t>(m_pending_active_id & 0xFF));
  payload.push_back(static_cast<uint8_t>(m_pending_active_id >> 8));
  payload.push_back(static_cast<uint8_t>(static_cast<uint16_t>(follow_id) & 0xFF));
  payload.push_back(static_cast<uint8_t>(static_cast<uint16_t>(follow_id) >> 8));
  payload.push_back(count);
  auto push_u16 = [&payload](uint16_t v) {
    payload.push_back(static_cast<uint8_t>(v & 0xFF));
    payload.push_back(static_cast<uint8_t>(v >> 8));
  };
  for (uint8_t i = 0; i < count; ++i) {
    const auto& b = m_pending_bboxes[i];
    // Normalize float [0,1] -> uint16 [0,65535]
    auto to_u16 = [](float v) -> uint16_t {
      int iv = static_cast<int>(v * 65535.0f + 0.5f);
      if (iv < 0) iv = 0;
      if (iv > 65535) iv = 65535;
      return static_cast<uint16_t>(iv);
    };
    push_u16(b.id);
    push_u16(to_u16(b.cx));
    push_u16(to_u16(b.cy));
    push_u16(to_u16(b.w));
    push_u16(to_u16(b.h));
    payload.push_back(b.tracked ? 1u : 0u);  // flags: bit0=tracked
  }
  m_console->debug("Emitting detection payload: {} bytes, active_id={}, count={}",
                   payload.size(), m_pending_active_id, count);
  m_data_cb(payload);
}

void HailoFollowBridge::on_udp_data(const uint8_t* data, std::size_t len) {
  try {
    std::string raw(reinterpret_cast<const char*>(data), len);
    auto j = nlohmann::json::parse(raw);
    std::lock_guard<std::mutex> lock(m_params_mutex);
    if (j.contains("params") && j["params"].is_object()) {
      const auto& params = j["params"];
      for (auto& [key, val] : params.items()) {
        if (val.is_number() && m_params.count(key)) {
          m_params[key] = val.get<float>();
        }
      }
      m_console->debug("Received sync from Python ({} params)", params.size());
    }
    // Parse available tracking IDs for QOpenHD follow widget
    if (j.contains("avail_ids") && j["avail_ids"].is_array()) {
      std::string ids_str;
      for (const auto& id : j["avail_ids"]) {
        if (id.is_number_integer()) {
          if (!ids_str.empty()) ids_str += ",";
          ids_str += std::to_string(id.get<int>());
        }
      }
      m_avail_ids_str = ids_str;
    }
    // Parse bounding boxes for TUNNEL overlay
    if (j.contains("bboxes") && j["bboxes"].is_array()) {
      m_console->debug("bboxes array received, size={}", j["bboxes"].size());
      m_pending_bboxes.clear();
      // active_id is reported in params["active_id"] (already in m_params)
      float active_f = 0.0f;
      auto it = m_params.find("active_id");
      if (it != m_params.end()) active_f = it->second;
      m_pending_active_id = static_cast<uint16_t>(
          std::max(0.0f, std::min(65535.0f, active_f)));
      for (const auto& bbox : j["bboxes"]) {
        if (!bbox.is_object()) continue;
        BboxEntry entry{};
        entry.id      = static_cast<uint16_t>(bbox.value("id", 0));
        entry.cx      = bbox.value("cx", 0.0f);
        entry.cy      = bbox.value("cy", 0.0f);
        entry.w       = bbox.value("w", 0.0f);
        entry.h       = bbox.value("h", 0.0f);
        entry.tracked = bbox.value("tracked", false);
        m_pending_bboxes.push_back(entry);
      }
      emit_data_if_cb_set();
    }
  } catch (const std::exception& e) {
    m_console->warn("Failed to parse Python report: {}", e.what());
  }
}

std::vector<openhd::Setting> HailoFollowBridge::get_all_settings() {
  std::vector<openhd::Setting> ret;
  for (const auto& def : get_param_defs()) {
    const bool is_float = (def.type == ParamType::FLOAT);
    // FLOAT params are scaled ×100 in MAVLink (5.0 → 500) so they can be
    // represented as integers while retaining 2 decimal places of precision.
    const int mavlink_default = is_float
        ? static_cast<int>(def.default_value * 100.0f)
        : static_cast<int>(def.default_value);
    auto change_cb = [this, name = def.python_name, is_float](std::string,
                                                               int value) -> bool {
      const float python_val = is_float ? value / 100.0f : static_cast<float>(value);
      set_param(name, python_val);
      send_param_to_python(name, python_val);
      return true;
    };
    auto get_cb = [this, name = def.python_name, is_float]() -> int {
      const float python_val = get_param(name);
      return is_float ? static_cast<int>(python_val * 100.0f)
                      : static_cast<int>(python_val);
    };
    ret.push_back(openhd::Setting{
        def.mavlink_id,
        openhd::IntSetting{mavlink_default, change_cb, get_cb}});
  }
  // DF_AVAIL_IDS: read-only string — comma-separated tracking IDs currently
  // visible in frame. Updated by the Python app's periodic report; read by
  // QOpenHD's drone follow widget to populate the ID selection list.
  ret.push_back(openhd::Setting{
      "DF_AVAIL_IDS",
      openhd::StringSetting{
          "",
          openhd::create_log_only_cb_string(),
          [this]() -> std::string {
            std::lock_guard<std::mutex> lock(m_params_mutex);
            return m_avail_ids_str;
          }}});
  return ret;
}
