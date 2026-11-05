// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Self-elevation through sudo for Windows (24H2+), Inline mode.
//
// ring0 needs admin: the kernel ETW session behind per-process disk and network
// I/O, handles to processes owned by other users and services, and suspend or
// priority changes on anything elevated. A requireAdministrator manifest would
// prompt on every launch and makes CreateProcess fail outright (740) for any
// launcher that does not go through ShellExecute. Instead an unelevated launch
// re-runs itself through sudo and waits; the elevated copy does the work.
//
// Same gate as MasterControlProgram: sudo must be in Inline mode, and the
// elevated child is marked with a sentinel so a sudo that returns without
// elevating can never cause a relaunch loop.

namespace ring0::elevate {

inline constexpr wchar_t kElevatedFlag[] = L"--elevated";

// Escape hatch for development. RING0_ALLOW_UNELEVATED=1 runs degraded
// instead of elevating or refusing.
inline constexpr wchar_t kAllowUnelevated[] = L"RING0_ALLOW_UNELEVATED";

enum class Gate { Run, Exit };

// Call first thing in main, before any window or thread exists. On Exit, the
// process should return `exit_code` immediately.
Gate gate(int& exit_code);

bool is_elevated();

} // namespace ring0::elevate
