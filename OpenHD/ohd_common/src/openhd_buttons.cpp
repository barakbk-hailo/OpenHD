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

#include "openhd_buttons.h"

#include <thread>

#include "openhd_platform.h"
#include "openhd_spdlog.h"
#include "openhd_util.h"

namespace openhd::rpi {

// RPI GPIO26: Used as a reset button

// After configure: Connect gpio26 to ground -> reports 0
// Do not connect gpio26 to ground -> reports 1
static void gpio26_configure() {
  OHDUtil::run_command("pinctrl", {"set", "26", "ip", "pu"});
};

static bool gpio26_user_wants_reset_frequencies() {
  const auto res_opt = OHDUtil::run_command_out("pinctrl get 26");
  if (res_opt) {
    const auto res = res_opt.value();
    // pinctrl output: "26: ip    pu | lo // GPIO26 = input"
    // "| lo" means level=low, i.e. GPIO26 is connected to ground
    if (OHDUtil::contains(res, "| lo")) {
      openhd::log::get_default()->info(
          "GPIO26 pull UP and level=low, user_wants_reset_frequencies");
      return true;
    }
  }
  return false;
};

}  // namespace openhd::rpi
openhd::ButtonManager& openhd::ButtonManager::instance() {
  static ButtonManager instance;
  return instance;
}

bool openhd::ButtonManager::user_wants_reset_openhd_core() {
  // Right now only supported on rpi
  if (OHDPlatform::instance().is_rpi()) {
    openhd::rpi::gpio26_configure();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto res_opt = OHDUtil::run_command_out("pinctrl get 26");
    if (res_opt) {
      const auto res = res_opt.value();
      // pinctrl output: "26: ip    pu | lo // GPIO26 = input"
      if (OHDUtil::contains(res, "| lo")) {
        openhd::log::get_default()->info(
            "GPIO26 pull UP and level=low, user_wants_reset_frequencies");
        return true;
      }
    }
  }
  return false;
}
