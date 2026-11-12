// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Asks local inference servers what they have loaded and how fast they are going.
// Requests go to 127.0.0.1 only, on ports exactly one process listens on, from a
// worker thread so a hung server never stalls the sampler.

namespace ring0 {

struct ProbeTarget {
    std::uint32_t pid = 0;
    std::wstring  image;       // lowercase image name, picks the endpoints
    std::wstring  api_key;     // from the process's own --api-key, sent only back to it
};

struct ProbeResult {
    float tokens_per_s = -1;
    int   busy_slots = -1;
    std::vector<std::wstring> loaded;   // names the server reports as resident
};

class ServerProbe {
public:
    ServerProbe();
    ~ServerProbe();
    ServerProbe(const ServerProbe&) = delete;
    ServerProbe& operator=(const ServerProbe&) = delete;

    void set_targets(std::vector<ProbeTarget> targets);
    std::unordered_map<std::uint32_t, ProbeResult> results() const;

    static bool wants_probe(const std::wstring& lower_image);

private:
    void run(std::stop_token st);

    mutable std::mutex mu_;
    std::vector<ProbeTarget> targets_;
    std::unordered_map<std::uint32_t, ProbeResult> results_;
    void* wake_ = nullptr;
    std::jthread thread_;
};

} // namespace ring0
