// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>

// The two processor knobs of a Windows power plan that bound package power:
// maximum processor state (percent) and boost mode. Both are written for AC and
// DC, so a cap holds on battery as well. Writes go to the plan itself, which
// persists across reboots, so callers are responsible for putting the original
// values back, into the plan they came from.

namespace ring0::power_plan {

using SchemeId = std::array<std::uint8_t, 16>;   // a GUID's bytes

struct Settings {
    SchemeId scheme{};
    std::uint32_t max_state_ac = 100;
    std::uint32_t max_state_dc = 100;
    std::uint32_t boost_ac = 2;   // 0 disabled, 1 enabled, 2 aggressive, ...
    std::uint32_t boost_dc = 2;
};

std::optional<SchemeId> active();

// Reads the active plan.
std::optional<Settings> read();

// Writes into the plan named by s.scheme, and re-applies it if it is the active
// one, since a change to the active plan takes effect only on re-application.
bool write(const Settings& s);

} // namespace ring0::power_plan
