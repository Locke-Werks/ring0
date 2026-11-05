// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sampler.hpp"

#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cwchar>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ring0 {
namespace {

using Microsoft::WRL::ComPtr;

// Resolved at runtime from gdi32 rather than linked, so a stripped-down system without
// the WDDM thunks degrades to an empty GPU slice instead of failing to load.
struct Kmt {
    decltype(&D3DKMTEnumAdapters2)    enum_adapters2 = nullptr;
    decltype(&D3DKMTQueryStatistics)  query_statistics = nullptr;
    decltype(&D3DKMTQueryAdapterInfo) query_adapter_info = nullptr;
    decltype(&D3DKMTCloseAdapter)     close_adapter = nullptr;

    bool load() {
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
        if (!gdi) return false;
        enum_adapters2     = reinterpret_cast<decltype(enum_adapters2)>(GetProcAddress(gdi, "D3DKMTEnumAdapters2"));
        query_statistics   = reinterpret_cast<decltype(query_statistics)>(GetProcAddress(gdi, "D3DKMTQueryStatistics"));
        query_adapter_info = reinterpret_cast<decltype(query_adapter_info)>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
        close_adapter      = reinterpret_cast<decltype(close_adapter)>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
        return enum_adapters2 && query_statistics;
    }
};

// EnumAdapters2 opens a kernel handle per adapter; the legacy code leaked them.
class AdapterHandle {
public:
    AdapterHandle() = default;
    AdapterHandle(D3DKMT_HANDLE h, decltype(&D3DKMTCloseAdapter) close) : h_(h), close_(close) {}
    AdapterHandle(const AdapterHandle&) = delete;
    AdapterHandle& operator=(const AdapterHandle&) = delete;
    AdapterHandle(AdapterHandle&& o) noexcept
        : h_(std::exchange(o.h_, 0)), close_(std::exchange(o.close_, nullptr)) {}
    AdapterHandle& operator=(AdapterHandle&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = std::exchange(o.h_, 0);
            close_ = std::exchange(o.close_, nullptr);
        }
        return *this;
    }
    ~AdapterHandle() { reset(); }
    D3DKMT_HANDLE get() const { return h_; }

private:
    void reset() {
        if (h_ && close_) {
            D3DKMT_CLOSEADAPTER c{};
            c.hAdapter = h_;
            close_(&c);
        }
        h_ = 0;
    }
    D3DKMT_HANDLE h_ = 0;
    decltype(&D3DKMTCloseAdapter) close_ = nullptr;
};

struct UniqueHandle {
    HANDLE h = nullptr;
    explicit UniqueHandle(HANDLE v) : h(v) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    ~UniqueHandle() { if (h) CloseHandle(h); }
};

struct UniqueModule {
    HMODULE m = nullptr;
    UniqueModule() = default;
    UniqueModule(const UniqueModule&) = delete;
    UniqueModule& operator=(const UniqueModule&) = delete;
    ~UniqueModule() { if (m) FreeLibrary(m); }
};

// NVAPI is not in any SDK; these ids and the struct layout are the ones the legacy build used.
struct NvThermalSettings {
    std::uint32_t version;
    std::uint32_t count;
    struct {
        std::int32_t controller;
        std::int32_t default_min;
        std::int32_t default_max;
        std::int32_t current;
        std::int32_t target;
    } sensor[3];
};
using NvQueryInterface = void* (__cdecl*)(unsigned int);
using NvThermalFn      = int (__cdecl*)(void*, int, NvThermalSettings*);

constexpr UINT kVendorNvidia   = 0x10DE;
constexpr UINT kVendorMicrosoft = 0x1414;
constexpr UINT kBasicRenderDevice = 0x8C;

struct Engine {
    std::wstring  name;
    UINT          node = 0;
    std::uint64_t prev_running = 0;
    bool          has_prev = false;
};

using PciKey = std::tuple<UINT, UINT, UINT, UINT, UINT, UINT, UINT>;

struct Adapter {
    PciKey                 pci{};
    AdapterHandle          handle;
    LUID                   luid{};
    std::wstring           name;
    UINT                   vendor = 0;
    UINT                   node_count = 0;
    std::uint64_t          dedicated_total = 0;
    std::uint64_t          shared_total = 0;
    std::vector<Engine>    engines;
    bool                   perfdata_ok = true;
};

const wchar_t* engine_type_name(UINT type) {
    switch (type) {
    case 1: return L"3D";
    case 2: return L"Video Decode";
    case 3: return L"Video Encode";
    case 4: return L"Video Processing";
    case 5: return L"Scene Assembly";
    case 6: return L"Copy";
    case 7: return L"Overlay";
    case 8: return L"Crypto";
    default: return nullptr;
    }
}

bool same_luid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

std::uint64_t luid_key(const LUID& l) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(l.HighPart)) << 32) | l.LowPart;
}

class PdhQuery {
public:
    PdhQuery() = default;
    PdhQuery(const PdhQuery&) = delete;
    PdhQuery& operator=(const PdhQuery&) = delete;
    ~PdhQuery() { if (q_) PdhCloseQuery(q_); }
    bool open() { return PdhOpenQueryW(nullptr, 0, &q_) == ERROR_SUCCESS || (q_ = nullptr, false); }
    PDH_HQUERY get() const { return q_; }

private:
    PDH_HQUERY q_ = nullptr;
};

// Parses "luid_0xHHHHHHHH_0xLLLLLLLL" starting at p; returns the packed LUID or 0.
std::uint64_t parse_luid(const wchar_t* p) {
    if (!p || std::wcsncmp(p, L"luid_", 5) != 0) return 0;
    wchar_t* end = nullptr;
    const auto hi = std::wcstoull(p + 5, &end, 16);
    if (!end || *end != L'_') return 0;
    const auto lo = std::wcstoull(end + 1, &end, 16);
    return (hi << 32) | (lo & 0xFFFFFFFFull);
}

// PdhGetFormattedCounterArray with the size probe and the buffer it fills.
std::vector<std::byte> read_array(PDH_HCOUNTER c, DWORD& count) {
    std::vector<std::byte> buf;
    count = 0;
    if (!c) return buf;
    DWORD bytes = 0;
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_LARGE, &bytes, &count, nullptr) != PDH_MORE_DATA) return buf;
    buf.resize(bytes);
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_LARGE, &bytes, &count,
            reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data())) != ERROR_SUCCESS) {
        buf.clear();
        count = 0;
    }
    return buf;
}

template <class F>
void for_each_item(PDH_HCOUNTER c, F&& f) {
    DWORD count = 0;
    auto buf = read_array(c, count);
    const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
            continue;
        if (items[i].FmtValue.largeValue < 0) continue;
        f(items[i].szName, static_cast<std::uint64_t>(items[i].FmtValue.largeValue));
    }
}

class GpuSampler final : public Sampler {
public:
    GpuSampler() {
        if (!kmt_.load()) return;
        enumerate();
        init_nvapi();
        init_pdh();
    }

    void sample(Snapshot& out, const Snapshot* prev, double dt_seconds) override {
        if (adapters_.empty()) return;
        const double dt_100ns = dt_seconds > 0 ? dt_seconds * 1e7 : 0;

        const bool pdh_ok = pdh_.get() && PdhCollectQueryData(pdh_.get()) == ERROR_SUCCESS;
        std::unordered_map<std::uint64_t, std::uint64_t> dedicated, shared;
        if (pdh_ok) {
            // Several phys_N instances can share one LUID on linked adapters, so sum them.
            for_each_item(c_dedicated_, [&](const wchar_t* n, std::uint64_t v) { dedicated[parse_luid(n)] += v; });
            for_each_item(c_shared_, [&](const wchar_t* n, std::uint64_t v) { shared[parse_luid(n)] += v; });
        }

        out.gpus.reserve(adapters_.size());
        float machine = 0;
        for (auto& a : adapters_) {
            Gpu g;
            g.name = a.name;
            sample_engines(a, g, dt_100ns);
            g.dedicated_total = a.dedicated_total;
            g.shared_total = a.shared_total;
            if (auto it = dedicated.find(luid_key(a.luid)); it != dedicated.end()) g.dedicated_used = it->second;
            if (auto it = shared.find(luid_key(a.luid)); it != shared.end()) g.shared_used = it->second;
            g.temp_c = sample_temp(a);
            machine = std::max(machine, g.total_pct);
            out.gpus.push_back(std::move(g));
        }
        out.gpu_total = machine;

        if (pdh_ok) {
            sample_processes(out, prev, dt_100ns);
            sample_process_memory(out);
        }
    }

private:
    void init_pdh() {
        if (adapters_.empty() || !pdh_.open()) return;
        if (PdhAddEnglishCounterW(pdh_.get(), L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &c_dedicated_) != ERROR_SUCCESS)
            c_dedicated_ = nullptr;
        if (PdhAddEnglishCounterW(pdh_.get(), L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &c_shared_) != ERROR_SUCCESS)
            c_shared_ = nullptr;
        // Local Usage is what a process has resident in VRAM. Dedicated Usage credits
        // every process that opens a shared surface with its full size, so a streamer
        // mapping desktop surfaces read 42 GB on a 24 GB card.
        if (PdhAddEnglishCounterW(pdh_.get(), L"\\GPU Process Memory(*)\\Local Usage", 0, &c_proc_dedicated_) != ERROR_SUCCESS)
            c_proc_dedicated_ = nullptr;
        if (PdhAddEnglishCounterW(pdh_.get(), L"\\GPU Engine(*)\\Utilization Percentage", 0, &c_engine_) != ERROR_SUCCESS)
            c_engine_ = nullptr;
        // Utilization is a rate; prime it so the first real sample has a baseline.
        PdhCollectQueryData(pdh_.get());
    }

    void sample_process_memory(Snapshot& out) const {
        std::unordered_map<std::uint32_t, std::uint64_t> by_pid;
        for_each_item(c_proc_dedicated_, [&](const wchar_t* n, std::uint64_t v) {
            if (!n || std::wcsncmp(n, L"pid_", 4) != 0) return;
            wchar_t* end = nullptr;
            const auto pid = std::wcstoul(n + 4, &end, 10);
            if (end == n + 4) return;
            by_pid[static_cast<std::uint32_t>(pid)] += v;
        });
        if (by_pid.empty()) return;
        for (auto& p : out.processes)
            if (auto it = by_pid.find(p.pid); it != by_pid.end()) p.gpu_dedicated = it->second;
    }

    void enumerate() {
        D3DKMT_ENUMADAPTERS2 e{};
        if (kmt_.enum_adapters2(&e) != 0 || e.NumAdapters == 0) return;
        std::vector<D3DKMT_ADAPTERINFO> infos(e.NumAdapters);
        e.pAdapters = infos.data();
        if (kmt_.enum_adapters2(&e) != 0) return;
        infos.resize(e.NumAdapters);

        ComPtr<IDXGIFactory1> factory;
        std::vector<ComPtr<IDXGIAdapter1>> dxgi_adapters;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            ComPtr<IDXGIAdapter1> a1;
            for (UINT i = 0; factory->EnumAdapters1(i, &a1) == S_OK; ++i)
                dxgi_adapters.push_back(std::move(a1));
        }

        for (const auto& info : infos) {
            AdapterHandle handle(info.hAdapter, kmt_.close_adapter);

            // DXGI is the only source of a display name and the software flag, and the
            // LUID is the join key between the two APIs.
            ComPtr<IDXGIAdapter1> match;
            DXGI_ADAPTER_DESC1 desc{};
            for (const auto& d : dxgi_adapters) {
                DXGI_ADAPTER_DESC1 dd{};
                if (SUCCEEDED(d->GetDesc1(&dd)) && same_luid(dd.AdapterLuid, info.AdapterLuid)) {
                    match = d;
                    desc = dd;
                    break;
                }
            }
            if (!match) continue;
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (desc.VendorId == kVendorMicrosoft && desc.DeviceId == kBasicRenderDevice) continue;

            // An indirect display driver (virtual monitors for streaming hosts) gets its own
            // LUID, but DXGI describes it with the identity of the GPU that renders for it, so
            // it reads as a second copy of that GPU. It has no engines or memory of its own.
            if (kmt_.query_adapter_info) {
                D3DKMT_ADAPTERTYPE type{};
                D3DKMT_QUERYADAPTERINFO qai{};
                qai.hAdapter = handle.get();
                qai.Type = KMTQAITYPE_ADAPTERTYPE;
                qai.pPrivateDriverData = &type;
                qai.PrivateDriverDataSize = sizeof(type);
                if (kmt_.query_adapter_info(&qai) == 0 &&
                    (!type.RenderSupported || type.IndirectDisplayDevice))
                    continue;
            }

            // Belt and braces for anything else that aliases one device: the same LUID, or
            // the same PCI identity at the same bus location, is one physical adapter.
            const auto pci = pci_key(handle.get(), desc);
            const bool dup = std::ranges::any_of(adapters_, [&](const Adapter& o) {
                return same_luid(o.luid, info.AdapterLuid) || (pci != PciKey{} && o.pci == pci);
            });
            if (dup) continue;

            Adapter a;
            a.handle = std::move(handle);
            a.luid = info.AdapterLuid;
            a.name = desc.Description;
            a.vendor = desc.VendorId;
            a.pci = pci;
            a.dedicated_total = desc.DedicatedVideoMemory;
            a.shared_total = desc.SharedSystemMemory;

            D3DKMT_QUERYSTATISTICS q{};
            q.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
            q.AdapterLuid = a.luid;
            if (kmt_.query_statistics(&q) != 0) continue;
            a.node_count = std::min<UINT>(q.QueryResult.AdapterInformation.NodeCount, 64);
            init_engines(a);
            adapters_.push_back(std::move(a));
        }
    }

    void init_engines(Adapter& a) {
        for (UINT n = 0; n < a.node_count && a.engines.size() < kMaxEngines; ++n) {
            Engine e;
            e.node = n;
            if (kmt_.query_adapter_info) {
                D3DKMT_NODEMETADATA md{};
                md.NodeOrdinalAndAdapterIndex = n;
                D3DKMT_QUERYADAPTERINFO qai{};
                qai.hAdapter = a.handle.get();
                qai.Type = KMTQAITYPE_NODEMETADATA;
                qai.pPrivateDriverData = &md;
                qai.PrivateDriverDataSize = sizeof(md);
                if (kmt_.query_adapter_info(&qai) == 0) {
                    if (md.NodeData.FriendlyName[0])
                        e.name = md.NodeData.FriendlyName;
                    else if (auto* t = engine_type_name(static_cast<UINT>(md.NodeData.EngineType)))
                        e.name = t;
                }
            }
            if (e.name.empty()) e.name = L"Engine " + std::to_wstring(n);
            a.engines.push_back(std::move(e));
        }
    }

    void init_nvapi() {
        const auto nvidia = std::ranges::count_if(adapters_, [](const Adapter& a) { return a.vendor == kVendorNvidia; });
        // NVAPI handles cannot be tied to a LUID without more of its API, so only trust it
        // when there is exactly one NVIDIA adapter to attribute the reading to.
        if (nvidia != 1) return;
        nvapi_.m = LoadLibraryW(L"nvapi64.dll");
        if (!nvapi_.m) return;
        auto qi = reinterpret_cast<NvQueryInterface>(GetProcAddress(nvapi_.m, "nvapi_QueryInterface"));
        if (!qi) return;
        using InitFn = int (__cdecl*)();
        using EnumFn = int (__cdecl*)(void* handles[64], int* count);
        auto init = reinterpret_cast<InitFn>(qi(0x0150E828));
        if (!init || init() != 0) return;
        auto enum_gpus = reinterpret_cast<EnumFn>(qi(0xE5AC921F));
        if (!enum_gpus) return;
        void* handles[64] = {};
        int count = 0;
        if (enum_gpus(handles, &count) != 0 || count == 0) return;
        nv_gpu_ = handles[0];
        nv_thermal_ = reinterpret_cast<NvThermalFn>(qi(0xE3640A56));
    }

    void sample_engines(Adapter& a, Gpu& g, double dt_100ns) {
        g.engines.reserve(a.engines.size());
        for (auto& e : a.engines) {
            D3DKMT_QUERYSTATISTICS q{};
            q.Type = D3DKMT_QUERYSTATISTICS_NODE;
            q.AdapterLuid = a.luid;
            q.QueryNode.NodeId = e.node;
            GpuEngine ge;
            ge.name = e.name;
            if (kmt_.query_statistics(&q) == 0) {
                const auto rt = static_cast<std::uint64_t>(
                    q.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart);
                if (e.has_prev && dt_100ns > 0 && rt >= e.prev_running)
                    ge.pct = static_cast<float>(std::clamp(
                        static_cast<double>(rt - e.prev_running) / dt_100ns * 100.0, 0.0, 100.0));
                e.prev_running = rt;
                e.has_prev = true;
            }
            g.total_pct = std::max(g.total_pct, ge.pct);
            g.engines.push_back(std::move(ge));
        }
    }

    // Zero when the bus location is unknown, so virtual adapters never collide on it.
    PciKey pci_key(D3DKMT_HANDLE h, const DXGI_ADAPTER_DESC1& d) const {
        if (!kmt_.query_adapter_info) return {};
        D3DKMT_ADAPTERADDRESS addr{};
        D3DKMT_QUERYADAPTERINFO qai{};
        qai.hAdapter = h;
        qai.Type = KMTQAITYPE_ADAPTERADDRESS;
        qai.pPrivateDriverData = &addr;
        qai.PrivateDriverDataSize = sizeof(addr);
        if (kmt_.query_adapter_info(&qai) != 0 || addr.BusNumber == 0xFFFFFFFF) return {};
        return {d.VendorId, d.DeviceId, d.SubSysId, d.Revision, addr.BusNumber, addr.DeviceNumber, addr.FunctionNumber};
    }

    float sample_temp(Adapter& a) {
        // Adapter perf data is what Task Manager reads and works across vendors on WDDM 2.4+;
        // NVAPI covers older NVIDIA drivers that report zero there.
        if (a.perfdata_ok && kmt_.query_adapter_info) {
            D3DKMT_ADAPTER_PERFDATA pd{};
            D3DKMT_QUERYADAPTERINFO qai{};
            qai.hAdapter = a.handle.get();
            qai.Type = KMTQAITYPE_ADAPTERPERFDATA;
            qai.pPrivateDriverData = &pd;
            qai.PrivateDriverDataSize = sizeof(pd);
            if (kmt_.query_adapter_info(&qai) == 0) {
                if (pd.Temperature > 0) return static_cast<float>(pd.Temperature) / 10.0f;
            } else {
                a.perfdata_ok = false;
            }
        }
        if (a.vendor == kVendorNvidia && nv_thermal_ && nv_gpu_) {
            NvThermalSettings ts{};
            ts.version = static_cast<std::uint32_t>(sizeof(ts)) | (1u << 16);
            if (nv_thermal_(nv_gpu_, 0, &ts) == 0 && ts.count > 0)
                return static_cast<float>(ts.sensor[0].current);
        }
        return -1;
    }

    // D3DKMT_QUERYSTATISTICS_PROCESS_NODE rejects a PROCESS_QUERY_LIMITED_INFORMATION handle
    // with STATUS_INVALID_PARAMETER, and with full query rights it returned negative running
    // times here; protected game processes refuse full rights anyway. The engine counter is
    // what Task Manager reads and needs no process handle at all.
    void sample_processes(Snapshot& out, const Snapshot* prev, double dt_100ns) const {
        if (!c_engine_) return;
        DWORD count = 0;
        DWORD bytes = 0;
        if (PdhGetFormattedCounterArrayW(c_engine_, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA) return;
        std::vector<std::byte> buf(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(c_engine_, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return;

        // Task Manager sums the instances of one engine type and shows the busiest type.
        std::unordered_map<std::uint32_t, std::unordered_map<std::wstring, double>> by_pid;
        for (DWORD i = 0; i < count; ++i) {
            const auto& it = items[i];
            if (it.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            if (it.FmtValue.doubleValue <= 0 || !it.szName || std::wcsncmp(it.szName, L"pid_", 4) != 0) continue;
            wchar_t* end = nullptr;
            const auto pid = std::wcstoul(it.szName + 4, &end, 10);
            if (end == it.szName + 4) continue;
            const wchar_t* type = std::wcsstr(it.szName, L"_engtype_");
            by_pid[static_cast<std::uint32_t>(pid)][type ? type + 9 : L""] += it.FmtValue.doubleValue;
        }

        std::unordered_map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> before;
        if (prev) {
            before.reserve(prev->processes.size());
            for (const auto& p : prev->processes)
                before.emplace(p.pid, std::pair{p.create_time, p.gpu_100ns});
        }

        for (auto& p : out.processes) {
            if (p.pid == 0) continue;
            double pct = 0;
            if (auto f = by_pid.find(p.pid); f != by_pid.end())
                for (const auto& [type, v] : f->second) pct = std::max(pct, v);
            pct = std::clamp(pct, 0.0, 100.0);
            p.gpu_pct = static_cast<float>(pct);

            // Integrated from the rate so the cumulative column survives the source change.
            auto b = before.find(p.pid);
            const std::uint64_t base = (b != before.end() && b->second.first == p.create_time) ? b->second.second : 0;
            p.gpu_100ns = base + static_cast<std::uint64_t>(pct / 100.0 * dt_100ns);
        }
    }


    Kmt                  kmt_;
    std::vector<Adapter> adapters_;
    UniqueModule         nvapi_;
    void*                nv_gpu_ = nullptr;
    NvThermalFn          nv_thermal_ = nullptr;
    PdhQuery             pdh_;
    PDH_HCOUNTER         c_dedicated_ = nullptr;
    PDH_HCOUNTER         c_shared_ = nullptr;
    PDH_HCOUNTER         c_proc_dedicated_ = nullptr;
    PDH_HCOUNTER         c_engine_ = nullptr;
};

} // namespace

std::unique_ptr<Sampler> make_gpu_sampler() { return std::make_unique<GpuSampler>(); }

} // namespace ring0
