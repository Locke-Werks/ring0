// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/evacuate.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <memory>
#include <unordered_set>

namespace ring0 {
namespace {

struct HandleCloser {
    void operator()(HANDLE h) const noexcept { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

UniqueHandle open(std::uint32_t pid, DWORD access)
{
    return UniqueHandle(OpenProcess(access, FALSE, pid));
}

std::uint64_t creation_time(HANDLE h)
{
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(h, &c, &e, &k, &u)) return 0;
    return (std::uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}

bool read_cpu_sets(HANDLE h, std::vector<unsigned long>& out)
{
    ULONG need = 0;
    if (GetProcessDefaultCpuSets(h, nullptr, 0, &need)) { out.clear(); return true; }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    out.resize(need);
    if (!GetProcessDefaultCpuSets(h, out.data(), need, &need)) return false;
    out.resize(need);
    return true;
}

// Idle and System cannot be touched and are not worth an access-denied every pass.
bool untouchable(std::uint32_t pid) noexcept { return pid == 0 || pid == 4; }

} // namespace

CoreEvacuator::CoreEvacuator()
{
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    std::vector<std::byte> buf(len);
    if (len && GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()),
                                          len, &len, GetCurrentProcess(), 0)) {
        for (ULONG off = 0; off < len;) {
            const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buf.data() + off);
            if (info->Type == CpuSetInformation)
                lps_.push_back({info->CpuSet.Id, info->CpuSet.Group, info->CpuSet.LogicalProcessorIndex});
            off += info->Size;
        }
    }
    std::ranges::sort(lps_, [](const Lp& a, const Lp& b) {
        return a.group != b.group ? a.group < b.group : a.index < b.index;
    });
    single_group_ = std::ranges::all_of(lps_, [](const Lp& l) { return l.group == 0; });
    excluded_.assign(lps_.size(), false);
}

CoreEvacuator::~CoreEvacuator()
{
    restore_all();
}

bool CoreEvacuator::any_excluded() const noexcept
{
    return std::ranges::any_of(excluded_, [](bool b) { return b; });
}

bool CoreEvacuator::set_excluded(const std::vector<bool>& excluded)
{
    std::vector<bool> next(lps_.size(), false);
    for (std::size_t i = 0; i < next.size() && i < excluded.size(); ++i) next[i] = excluded[i];
    if (!next.empty() && std::ranges::all_of(next, [](bool b) { return b; })) return false;

    excluded_ = std::move(next);
    if (!any_excluded()) {
        restore_all();
        return true;
    }
    for (auto& [pid, rec] : records_) apply(pid, &rec);
    sweep();
    return true;
}

void CoreEvacuator::sweep()
{
    if (!any_excluded()) return;
    UniqueHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap || snap.get() == INVALID_HANDLE_VALUE) return;

    std::unordered_set<std::uint32_t> alive;
    PROCESSENTRY32W pe{.dwSize = sizeof pe};
    for (BOOL ok = Process32FirstW(snap.get(), &pe); ok; ok = Process32NextW(snap.get(), &pe)) {
        const std::uint32_t pid = pe.th32ProcessID;
        if (untouchable(pid)) continue;
        alive.insert(pid);
        if (!records_.contains(pid)) apply(pid, nullptr);
    }
    std::erase_if(records_, [&](const auto& kv) { return !alive.contains(kv.first); });
}

void CoreEvacuator::apply(std::uint32_t pid, Record* existing)
{
    Record fresh;
    Record& r = existing ? *existing : fresh;

    // A pinned process that was refused earlier may be allowed now that the
    // exclusion changed, so refused records are retried from scratch.
    if (existing && r.method == Method::Refused) r = Record{};

    if (r.method != Method::Affinity) {
        if (auto h = open(pid, PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_LIMITED_INFORMATION)) {
            const std::uint64_t created = creation_time(h.get());
            bool have_orig = r.method == Method::CpuSets && r.created == created;
            if (!have_orig) {
                r = Record{.created = created};
                have_orig = read_cpu_sets(h.get(), r.orig_sets);
            }
            if (have_orig) {
                std::vector<unsigned long> allowed;
                for (std::size_t i = 0; i < lps_.size(); ++i) {
                    if (excluded_[i]) continue;
                    const unsigned long id = lps_[i].cpu_set_id;
                    if (r.orig_sets.empty() || std::ranges::find(r.orig_sets, id) != r.orig_sets.end())
                        allowed.push_back(id);
                }
                if (!allowed.empty() &&
                    SetProcessDefaultCpuSets(h.get(), allowed.data(), static_cast<ULONG>(allowed.size()))) {
                    r.method = Method::CpuSets;
                    if (!existing) records_.emplace(pid, std::move(fresh));
                    return;
                }
            }
        }
    }

    // A process already steered by CPU Sets keeps that method; switching it to a
    // mask would lose the original CPU sets that restore needs.
    if (single_group_ && r.method != Method::CpuSets) {
        if (auto h = open(pid, PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION)) {
            const std::uint64_t created = creation_time(h.get());
            if (r.method != Method::Affinity || r.created != created) {
                DWORD_PTR proc = 0, sys = 0;
                if (GetProcessAffinityMask(h.get(), &proc, &sys)) {
                    r = Record{.created = created, .method = Method::Refused, .orig_mask = proc};
                }
            }
            if (r.orig_mask) {
                DWORD_PTR mask = r.orig_mask;
                for (std::size_t i = 0; i < lps_.size(); ++i)
                    if (excluded_[i]) mask &= ~(DWORD_PTR(1) << lps_[i].index);
                if (mask && SetProcessAffinityMask(h.get(), mask)) {
                    r.method = Method::Affinity;
                    if (!existing) records_.emplace(pid, std::move(fresh));
                    return;
                }
            }
        }
    }

    // Record the refusal so the next sweep does not knock on the same door, but
    // leave any original state intact: a process moved earlier is still moved.
    if (r.method == Method::CpuSets || r.method == Method::Affinity) {
        if (existing) restore(pid, r);
    }
    r.method = Method::Refused;
    if (!existing) records_.emplace(pid, std::move(fresh));
}

void CoreEvacuator::restore(std::uint32_t pid, const Record& r)
{
    if (r.method == Method::CpuSets) {
        if (auto h = open(pid, PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_LIMITED_INFORMATION)) {
            if (r.created && creation_time(h.get()) != r.created) return;
            SetProcessDefaultCpuSets(h.get(), r.orig_sets.empty() ? nullptr : r.orig_sets.data(),
                                     static_cast<ULONG>(r.orig_sets.size()));
        }
    } else if (r.method == Method::Affinity) {
        if (auto h = open(pid, PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION)) {
            if (r.created && creation_time(h.get()) != r.created) return;
            SetProcessAffinityMask(h.get(), r.orig_mask);
        }
    }
}

void CoreEvacuator::restore_all()
{
    for (const auto& [pid, rec] : records_) restore(pid, rec);
    records_.clear();
    std::ranges::fill(excluded_, false);
}

CoreEvacuator::Stats CoreEvacuator::stats() const noexcept
{
    Stats s;
    for (const auto& [pid, rec] : records_) {
        if (rec.method == Method::Refused) ++s.refused;
        else ++s.moved;
    }
    return s;
}

} // namespace ring0
