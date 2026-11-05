// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>

// One clock for the sampler and every renderer. Graphs position each sample by
// its own timestamp against this clock, so their motion is independent of when
// a sample happens to be delivered.
namespace ring0::timebase {

inline constexpr double kPeriodMs = 1000.0;

// How far behind "now" the graphs draw. It has to exceed one period plus the
// worst delivery latency, or the right edge would run ahead of the newest data.
inline constexpr double kRenderDelayMs = 1250.0;

inline double now_ms()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

} // namespace ring0::timebase
