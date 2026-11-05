// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sampler.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "iphlpapi.lib")

namespace ring0 {
namespace {

struct MibTableDeleter {
    void operator()(MIB_IF_TABLE2* t) const noexcept { if (t) FreeMibTable(t); }
};
using IfTable = std::unique_ptr<MIB_IF_TABLE2, MibTableDeleter>;

struct Counters {
    std::uint64_t in = 0, out = 0, ms = 0;
};

float rate(std::uint64_t now, std::uint64_t then, double secs) {
    if (now < then || secs <= 0) return 0.0f;
    return static_cast<float>(static_cast<double>(now - then) / secs);
}

class NetSampler final : public Sampler {
public:
    void sample(Snapshot& out, const Snapshot*, double) override {
        out.ifaces.clear();
        out.net_bps_total = 0;

        MIB_IF_TABLE2* raw = nullptr;
        if (GetIfTable2(&raw) != NO_ERROR || !raw) return;
        const IfTable tbl{raw};
        const std::uint64_t now_ms = GetTickCount64();

        std::unordered_set<std::uint64_t> seen;
        for (ULONG i = 0; i < tbl->NumEntries; ++i) {
            const MIB_IF_ROW2& row = tbl->Table[i];
            if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            // Filters out the WFP/QoS/LWF filter rows that duplicate each real NIC.
            if (!row.InterfaceAndOperStatusFlags.HardwareInterface) continue;
            if (row.MediaConnectState != MediaConnectStateConnected) continue;

            const std::uint64_t luid = row.InterfaceLuid.Value;
            seen.insert(luid);

            NetIf n;
            n.name     = row.Alias;
            n.link_bps = std::min(row.ReceiveLinkSpeed, row.TransmitLinkSpeed);

            if (const auto it = prev_.find(luid); it != prev_.end() && now_ms > it->second.ms) {
                const double secs = static_cast<double>(now_ms - it->second.ms) / 1000.0;
                n.rx_bps = rate(row.InOctets, it->second.in, secs);
                n.tx_bps = rate(row.OutOctets, it->second.out, secs);
            }
            prev_[luid] = Counters{row.InOctets, row.OutOctets, now_ms};

            out.net_bps_total += n.rx_bps + n.tx_bps;
            out.ifaces.push_back(std::move(n));
        }

        // Unplugged adapters would otherwise pin stale counters and produce a
        // bogus spike if the same LUID reconnects much later.
        std::erase_if(prev_, [&](const auto& kv) { return !seen.contains(kv.first); });
    }

private:
    std::unordered_map<std::uint64_t, Counters> prev_;
};

} // namespace

std::unique_ptr<Sampler> make_net_sampler() { return std::make_unique<NetSampler>(); }

} // namespace ring0
