// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/trust.hpp"

#include <cstdint>
#include <string>
#include <vector>

// One-shot deep look at a single process: identity, ancestry, loaded modules with
// their signatures, sockets, and a short list of plain-language findings. Every part
// degrades independently, so an unelevated caller still gets whatever the OS allows.

namespace ring0 {

struct ModuleInfo {
    std::wstring path;
    std::uint64_t base = 0;
    std::uint32_t size = 0;
    TrustInfo trust;
    bool user_writable_dir = false;
};

struct Connection {
    std::wstring proto;    // "tcp", "tcp6", "udp", "udp6"
    std::wstring local;    // "addr:port", IPv6 as "[addr]:port"
    std::wstring remote;   // empty for udp
    std::wstring state;    // lowercase MIB_TCP_STATE name; empty for udp
};

struct ProcessDetail {
    std::uint32_t pid = 0, ppid = 0;
    std::wstring image_path, command_line, user;   // user as DOMAIN\name
    std::wstring integrity;                        // "low","medium","high","system","protected", or ""
    bool elevated = false;
    std::wstring arch;                             // "x64","x86","arm64"
    std::uint64_t create_time = 0;                 // 100ns since 1601
    TrustInfo image_trust;
    std::vector<std::uint32_t> ancestry;           // nearest parent first; stops at a dead or reused pid
    std::vector<ModuleInfo> modules;
    std::vector<Connection> connections;
    std::vector<std::wstring> findings;
    std::wstring error;                            // why parts are missing; empty if complete
};

// Synchronous and potentially slow (signature checks on cold modules). Never call it
// from the UI thread. Never throws.
ProcessDetail inspect_process(std::uint32_t pid);

} // namespace ring0
