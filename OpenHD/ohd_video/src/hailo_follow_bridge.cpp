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

#include <fstream>
#include <pwd.h>

#include "include_json.hpp"
#include "openhd_spdlog_include.h"
#include "openhd_udp.h"

using PT = HailoFollowBridge::ParamType;

// Resolve the real (non-root) user's home directory.
// OpenHD runs under sudo, so $HOME is /root. Use $SUDO_USER to find the
// invoking user, then look up their home via /etc/passwd.
static std::string get_real_user_home() {
  const char* sudo_user = std::getenv("SUDO_USER");
  if (sudo_user) {
    struct passwd* pw = getpwnam(sudo_user);
    if (pw && pw->pw_dir) return pw->pw_dir;
  }
  // Fallback: try HOME (works when not under sudo)
  const char* home = std::getenv("HOME");
  if (home) return home;
  return "/home/pi";  // last resort
}

// Search paths for df_params.json (first found wins)
static std::vector<std::string> build_schema_search_paths() {
  return {
    "/usr/local/share/openhd/df_params.json",
    get_real_user_home() + "/hailo-drone-follow/df_params.json",
  };
}

// Load param definitions from df_params.json.
// Falls back to a minimal hardcoded set if the file is not found.
static std::vector<HailoFollowBridge::ParamDef> load_param_defs_from_json(
    std::shared_ptr<spdlog::logger> console) {
  std::vector<HailoFollowBridge::ParamDef> defs;

  std::string found_path;
  const auto search_paths = build_schema_search_paths();
  for (const auto& path : search_paths) {
    std::ifstream f(path);
    if (f.good()) {
      found_path = path;
      break;
    }
  }

  if (found_path.empty()) {
    console->warn("df_params.json not found in any search path, using hardcoded defaults");
    // Minimal fallback so the bridge still works
    defs.push_back({"DF_KP_YAW", "kp_yaw", PT::FLOAT, 5.0f});
    defs.push_back({"DF_KP_FWD", "kp_forward", PT::FLOAT, 3.0f});
    defs.push_back({"DF_FOLLOW_ID", "follow_id", PT::INT, 0});
    defs.push_back({"DF_ACTIVE_ID", "active_id", PT::INT, 0});
    defs.push_back({"DF_BITRATE", "bitrate_kbps", PT::INT, 3917});
    return defs;
  }

  try {
    std::ifstream f(found_path);
    auto j = nlohmann::json::parse(f);
    console->info("Loaded df_params.json from {}", found_path);

    for (const auto& p : j["params"]) {
      HailoFollowBridge::ParamDef def;
      def.mavlink_id = p["mavlink_id"].get<std::string>();
      def.python_name = p["id"].get<std::string>();

      const std::string type_str = p["type"].get<std::string>();
      if (type_str == "float") {
        def.type = PT::FLOAT;
        def.default_value = p["default"].get<float>();
      } else if (type_str == "bool") {
        def.type = PT::INT;
        def.default_value = p["default"].get<bool>() ? 1.0f : 0.0f;
      } else {
        // int
        def.type = PT::INT;
        def.default_value = static_cast<float>(p["default"].get<int>());
      }

      defs.push_back(def);
      console->debug("  param: {} -> {} ({})", def.mavlink_id, def.python_name,
                      type_str);
    }
    console->info("Loaded {} param definitions from df_params.json", defs.size());
  } catch (const std::exception& e) {
    console->error("Failed to parse df_params.json: {}", e.what());
    // Return whatever we managed to parse
  }
  return defs;
}

// Cached param defs, loaded once on first access
static std::vector<HailoFollowBridge::ParamDef> s_param_defs;
static bool s_param_defs_loaded = false;

const std::vector<HailoFollowBridge::ParamDef>&
HailoFollowBridge::get_param_defs() {
  // s_param_defs is populated in the constructor before any other access
  return s_param_defs;
}

HailoFollowBridge::HailoFollowBridge() {
  m_console = openhd::log::create_or_get("hailo_bridge");
  m_console->info("HailoFollowBridge starting");

  // Load param definitions from df_params.json (once)
  if (!s_param_defs_loaded) {
    s_param_defs = load_param_defs_from_json(m_console);
    s_param_defs_loaded = true;
  }

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

void HailoFollowBridge::update_param(const std::string& python_name,
                                      float value) {
  set_param(python_name, value);
  send_param_to_python(python_name, value);
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
    // Parse bounding boxes for detection overlay
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
  return ret;
}
