// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// Authenticode verification of image files, embedded signature first and then the
// system catalogs, which is where most in-box binaries carry theirs.

namespace ring0 {

enum class TrustState { Unknown, Signed, Unsigned, Invalid };

struct TrustInfo {
    TrustState state = TrustState::Unknown;
    std::wstring signer;   // leaf certificate display name; empty unless Signed
};

// Blocks for as long as WinVerifyTrust takes, which can be hundreds of milliseconds
// on a cold catalog. Never call it from the UI thread.
TrustInfo verify(std::wstring_view path);

// Thread-safe memo over verify(). request() queues a path for a single background
// worker; lookup() never blocks on verification and answers Unknown until it is done.
class TrustCache {
public:
    TrustCache();
    ~TrustCache();
    TrustCache(const TrustCache&) = delete;
    TrustCache& operator=(const TrustCache&) = delete;

    void request(std::wstring_view path);
    TrustInfo lookup(std::wstring_view path) const;

private:
    void run(std::stop_token stop);

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::unordered_map<std::wstring, TrustInfo> done_;
    std::unordered_set<std::wstring> queued_;
    std::deque<std::wstring> pending_;
    std::jthread worker_;   // last, so it starts after and stops before the state above
};

} // namespace ring0
