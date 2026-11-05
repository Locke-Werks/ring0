// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

// One-shot user-mode stack capture of every thread in a process via dbghelp.
// Each thread is suspended only while its stack is walked; symbols are resolved
// after all threads have been resumed.

namespace ring0 {

struct StackFrame {
    std::wstring  module;   // module base name, or "?" when the address is in no module
    std::wstring  symbol;   // undecorated name; empty when no symbol covers the address
    std::uint64_t offset = 0;   // displacement from symbol, or from module base if no symbol
    std::uint64_t address = 0;
};

struct ThreadStack {
    std::uint32_t tid = 0;
    std::vector<StackFrame> frames;   // innermost first
};

// Serialized internally: dbghelp is not thread-safe, so concurrent callers queue.
std::expected<std::vector<ThreadStack>, std::wstring> capture_stacks(std::uint32_t pid);

} // namespace ring0
