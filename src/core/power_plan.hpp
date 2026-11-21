// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>

// The two processor knobs of the active Windows power plan that bound package
// power: maximum processor state (percent) and boost mode. Both are written for
// AC and DC, so the cap holds on battery as well. Writes go to the plan itself,
// which persists across reboots, so callers are responsible for putting the
// original values back.

namespace ring0::power_plan {

struct Settings {
    std::uint32_t max_state_ac = 100;
    std::uint32_t max_state_dc = 100;
    std::uint32_t boost_ac = 2;   // 0 disabled, 1 enabled, 2 aggressive, ...
    std::uint32_t boost_dc = 2;
};

std::optional<Settings> read();
bool write(const Settings& s);

} // namespace ring0::power_plan
