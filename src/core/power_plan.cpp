// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/power_plan.hpp"

#include <windows.h>
#include <powrprof.h>

#include <cstring>

namespace ring0::power_plan {
namespace {

// Spelled out rather than taken from winnt.h, which only declares these GUIDs
// unless INITGUID is set, and no import library defines them.
constexpr GUID kProcessorSubgroup = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
constexpr GUID kMaxState          = {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};
constexpr GUID kBoostMode         = {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};

GUID to_guid(const SchemeId& id)
{
    GUID g;
    std::memcpy(&g, id.data(), sizeof g);
    return g;
}

SchemeId to_id(const GUID& g)
{
    SchemeId id;
    std::memcpy(id.data(), &g, sizeof g);
    return id;
}

} // namespace

std::optional<SchemeId> active()
{
    GUID* g = nullptr;
    if (PowerGetActiveScheme(nullptr, &g) != ERROR_SUCCESS || !g) return std::nullopt;
    const SchemeId id = to_id(*g);
    LocalFree(g);
    return id;
}

std::optional<Settings> read()
{
    const auto id = active();
    if (!id) return std::nullopt;
    const GUID g = to_guid(*id);
    Settings out;
    out.scheme = *id;
    DWORD v = 0;
    if (PowerReadACValueIndex(nullptr, &g, &kProcessorSubgroup, &kMaxState, &v) != ERROR_SUCCESS) return std::nullopt;
    out.max_state_ac = v;
    if (PowerReadDCValueIndex(nullptr, &g, &kProcessorSubgroup, &kMaxState, &v) == ERROR_SUCCESS) out.max_state_dc = v;
    if (PowerReadACValueIndex(nullptr, &g, &kProcessorSubgroup, &kBoostMode, &v) == ERROR_SUCCESS) out.boost_ac = v;
    if (PowerReadDCValueIndex(nullptr, &g, &kProcessorSubgroup, &kBoostMode, &v) == ERROR_SUCCESS) out.boost_dc = v;
    return out;
}

bool write(const Settings& in)
{
    const GUID g = to_guid(in.scheme);
    bool ok = PowerWriteACValueIndex(nullptr, &g, &kProcessorSubgroup, &kMaxState, in.max_state_ac) == ERROR_SUCCESS;
    PowerWriteDCValueIndex(nullptr, &g, &kProcessorSubgroup, &kMaxState, in.max_state_dc);
    PowerWriteACValueIndex(nullptr, &g, &kProcessorSubgroup, &kBoostMode, in.boost_ac);
    PowerWriteDCValueIndex(nullptr, &g, &kProcessorSubgroup, &kBoostMode, in.boost_dc);
    if (ok && active() == in.scheme) ok = PowerSetActiveScheme(nullptr, &g) == ERROR_SUCCESS;
    return ok;
}

} // namespace ring0::power_plan
