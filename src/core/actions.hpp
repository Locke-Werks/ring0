// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

// Operations a user can apply to a process from the UI. Each opens its own handle
// with the narrowest access it needs, so a failure names the exact right that was
// refused. Pid 0 (Idle) and pid 4 (System) are always refused.

namespace ring0 {

enum class PriorityClass : std::uint32_t {
    Idle        = 0x00000040,  // IDLE_PRIORITY_CLASS
    BelowNormal = 0x00004000,  // BELOW_NORMAL_PRIORITY_CLASS
    Normal      = 0x00000020,  // NORMAL_PRIORITY_CLASS
    AboveNormal = 0x00008000,  // ABOVE_NORMAL_PRIORITY_CLASS
    High        = 0x00000080,  // HIGH_PRIORITY_CLASS
    Realtime    = 0x00000100,  // REALTIME_PRIORITY_CLASS
};

using ActionResult = std::expected<void, std::wstring>;

ActionResult terminate_process(std::uint32_t pid, std::uint32_t exit_code = 1);
ActionResult suspend_process(std::uint32_t pid);
ActionResult resume_process(std::uint32_t pid);
ActionResult set_priority(std::uint32_t pid, PriorityClass cls);
std::expected<PriorityClass, std::wstring> get_priority(std::uint32_t pid);
ActionResult restrict_to_single_core(std::uint32_t pid);

// Opens Explorer with the image selected. Takes the path rather than a pid because
// the snapshot already resolved it, and a protected process will not yield it again.
ActionResult open_file_location(std::wstring_view image_path);

} // namespace ring0
