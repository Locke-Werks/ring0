// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/power_plan.hpp"

#include <windows.h>
#include <powrprof.h>

namespace ring0::power_plan {
namespace {

// Spelled out rather than taken from winnt.h, which only declares these GUIDs
// unless INITGUID is set, and no import library defines them.
constexpr GUID kProcessorSubgroup = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
constexpr GUID kMaxState          = {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};
constexpr GUID kBoostMode         = {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};

struct Scheme {
    GUID* guid = nullptr;
    Scheme() { if (PowerGetActiveScheme(nullptr, &guid) != ERROR_SUCCESS) guid = nullptr; }
    ~Scheme() { if (guid) LocalFree(guid); }
    Scheme(const Scheme&) = delete;
    Scheme& operator=(const Scheme&) = delete;
};

} // namespace

std::optional<Settings> read()
{
    Scheme s;
    if (!s.guid) return std::nullopt;
    Settings out;
    DWORD v = 0;
    if (PowerReadACValueIndex(nullptr, s.guid, &kProcessorSubgroup, &kMaxState, &v) != ERROR_SUCCESS) return std::nullopt;
    out.max_state_ac = v;
    if (PowerReadDCValueIndex(nullptr, s.guid, &kProcessorSubgroup, &kMaxState, &v) == ERROR_SUCCESS) out.max_state_dc = v;
    if (PowerReadACValueIndex(nullptr, s.guid, &kProcessorSubgroup, &kBoostMode, &v) == ERROR_SUCCESS) out.boost_ac = v;
    if (PowerReadDCValueIndex(nullptr, s.guid, &kProcessorSubgroup, &kBoostMode, &v) == ERROR_SUCCESS) out.boost_dc = v;
    return out;
}

bool write(const Settings& in)
{
    Scheme s;
    if (!s.guid) return false;
    bool ok = PowerWriteACValueIndex(nullptr, s.guid, &kProcessorSubgroup, &kMaxState, in.max_state_ac) == ERROR_SUCCESS;
    PowerWriteDCValueIndex(nullptr, s.guid, &kProcessorSubgroup, &kMaxState, in.max_state_dc);
    PowerWriteACValueIndex(nullptr, s.guid, &kProcessorSubgroup, &kBoostMode, in.boost_ac);
    PowerWriteDCValueIndex(nullptr, s.guid, &kProcessorSubgroup, &kBoostMode, in.boost_dc);
    // Values written to the active plan take effect only once it is re-applied.
    ok = ok && PowerSetActiveScheme(nullptr, s.guid) == ERROR_SUCCESS;
    return ok;
}

} // namespace ring0::power_plan
