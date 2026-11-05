// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sampler.hpp"
#include "core/pawnio.hpp"

#include <windows.h>
#include <intrin.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

#pragma comment(lib, "pdh.lib")

namespace ring0 {
namespace {

// Register layouts follow LibreHardwareMonitor's Amd17Cpu.cs and IntelCpu.cs, which in turn
// follow Linux k10temp and the Intel SDM.
constexpr std::uint64_t kThmTconCurTmp = 0x00059800; // AMD SMN, THM_TCON_CUR_TMP
constexpr std::uint32_t kTempRangeSel = 0x00080000; // bit 19
constexpr std::uint32_t kTempTjSelMask = 0x00030000; // bits 17:16
constexpr std::uint64_t kIa32PackageThermStatus = 0x1B1;
constexpr std::uint64_t kIa32TemperatureTarget = 0x1A2;

enum class Vendor { Other, Amd, Intel };

struct CpuInfo {
    Vendor vendor = Vendor::Other;
    unsigned family = 0;
    bool intel_pkg_sensor = false;
    std::array<char, 49> brand{};
};

CpuInfo identify() noexcept {
    CpuInfo info;
    std::array<int, 4> r{};
    __cpuid(r.data(), 0);
    const int max_leaf = r[0];
    char v[13]{};
    std::memcpy(v + 0, &r[1], 4);
    std::memcpy(v + 4, &r[3], 4);
    std::memcpy(v + 8, &r[2], 4);
    const std::string_view vendor(v);
    if (vendor == "AuthenticAMD") info.vendor = Vendor::Amd;
    else if (vendor == "GenuineIntel") info.vendor = Vendor::Intel;

    __cpuid(r.data(), 1);
    const unsigned base = (static_cast<unsigned>(r[0]) >> 8) & 0xF;
    const unsigned ext = (static_cast<unsigned>(r[0]) >> 20) & 0xFF;
    info.family = base == 0xF ? base + ext : base;

    // CPUID.06H:EAX[6] is the package thermal management bit; without it MSR 0x1B1 is absent.
    if (max_leaf >= 6) {
        __cpuid(r.data(), 6);
        info.intel_pkg_sensor = (r[0] & 0x40) != 0;
    }

    __cpuid(r.data(), static_cast<int>(0x80000000));
    if (static_cast<unsigned>(r[0]) >= 0x80000004) {
        for (int i = 0; i < 3; ++i) {
            __cpuid(r.data(), static_cast<int>(0x80000002 + i));
            std::memcpy(info.brand.data() + i * 16, r.data(), 16);
        }
    }
    return info;
}

// Tctl on these parts reads hotter than the die so fan curves ramp early; the offsets
// are the k10temp table LHM copies. Every later SKU reports Tctl equal to Tdie.
float amd_tdie_offset(std::string_view brand) noexcept {
    if (brand.contains("1600X") || brand.contains("1700X") || brand.contains("1800X")) return -20.0f;
    if (brand.contains("Threadripper 19") || brand.contains("Threadripper 29")) return -27.0f;
    if (brand.contains("2700X")) return -10.0f;
    return 0.0f;
}

std::filesystem::path module_path(const wchar_t* file) noexcept {
    std::array<wchar_t, MAX_PATH> exe{};
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (n == 0 || n >= exe.size()) return {};
    try {
        return std::filesystem::path(exe.data()).parent_path() / L"pawnio" / file;
    } catch (...) {
        return {};
    }
}

struct PdhQueryCloser {
    void operator()(PDH_HQUERY q) const noexcept { PdhCloseQuery(q); }
};
using PdhQuery = std::unique_ptr<std::remove_pointer_t<PDH_HQUERY>, PdhQueryCloser>;

struct HandleCloser {
    void operator()(HANDLE h) const noexcept { CloseHandle(h); }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

class ThermalSampler final : public Sampler {
public:
    ThermalSampler() {
        const CpuInfo cpu = identify();
        if (cpu.vendor == Vendor::Amd && cpu.family >= 0x17 && cpu.family <= 0x1A)
            init_amd(cpu);
        else if (cpu.vendor == Vendor::Intel && cpu.intel_pkg_sensor)
            init_intel();

        if (!module_) init_pdh();
    }

    void sample(Snapshot& out, const Snapshot*, double) override {
        out.cpu_temp_c = -1;
        if (module_) {
            out.cpu_temp_c = amd_ ? read_amd() : read_intel();
            return;
        }
        out.cpu_temp_c = read_pdh();
    }

private:
    void init_amd(const CpuInfo& cpu) {
        auto m = pawnio::Module::load_file(module_path(L"AMDFamily17.bin"));
        if (!m) return;
        module_ = std::move(*m);
        amd_ = true;
        tdie_offset_ = amd_tdie_offset(cpu.brand.data());
        // SMN goes through the PCI config index/data pair at 0:0.0 offsets 0x60/0x64. Other
        // monitors share that pair, and the module says to hold this mutex so an interleaved
        // index write from another process cannot redirect our data read.
        pci_mutex_.reset(CreateMutexW(nullptr, FALSE, L"Global\\Access_PCI"));
        if (!pci_mutex_) pci_mutex_.reset(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                                     L"Global\\Access_PCI"));
    }

    void init_intel() {
        auto m = pawnio::Module::load_file(module_path(L"IntelMSR.bin"));
        if (!m) return;
        module_ = std::move(*m);
        std::array<std::uint64_t, 1> in{kIa32TemperatureTarget};
        std::array<std::uint64_t, 1> out{};
        // TjMax is fused and does not change at runtime; 100 C is the documented fallback LHM uses.
        if (auto r = module_.execute("ioctl_read_msr", in, out); r && *r == 1) {
            const unsigned tj = static_cast<unsigned>(out[0] >> 16) & 0xFF;
            if (tj != 0) tjmax_ = static_cast<float>(tj);
        }
    }

    float read_amd() noexcept {
        if (pci_mutex_) {
            const DWORD w = WaitForSingleObject(pci_mutex_.get(), 10);
            if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return -1;
        }
        std::array<std::uint64_t, 1> in{kThmTconCurTmp};
        std::array<std::uint64_t, 1> out{};
        auto r = module_.execute("ioctl_read_smn", in, out);
        if (pci_mutex_) ReleaseMutex(pci_mutex_.get());
        if (!r || *r != 1) return -1;

        const auto raw = static_cast<std::uint32_t>(out[0]);
        // CUR_TEMP is bits 31:21 in 0.125 C steps. With range select set the scale starts at
        // -49 C so the sensor can report below zero; newer parts flag the same via TJ_SEL.
        float t = static_cast<float>(raw >> 21) * 0.125f;
        if ((raw & kTempRangeSel) != 0 || (raw & kTempTjSelMask) == kTempTjSelMask) t -= 49.0f;
        return t + tdie_offset_;
    }

    float read_intel() noexcept {
        std::array<std::uint64_t, 1> in{kIa32PackageThermStatus};
        std::array<std::uint64_t, 1> out{};
        auto r = module_.execute("ioctl_read_msr", in, out);
        if (!r || *r != 1) return -1;
        const auto v = static_cast<std::uint32_t>(out[0]);
        if ((v & 0x80000000u) == 0) return -1; // reading-valid bit
        // Bits 22:16 are degrees below TjMax, not an absolute temperature.
        return tjmax_ - static_cast<float>((v >> 16) & 0x7F);
    }

    void init_pdh() {
        PDH_HQUERY raw = nullptr;
        if (PdhOpenQueryW(nullptr, 0, &raw) != ERROR_SUCCESS) return;
        query_.reset(raw);

        // The wildcard picks up every ACPI thermal zone; zone 0 is usually the CPU
        // package but others may be chipset or NVMe, so the hottest one is reported.
        if (PdhAddEnglishCounterW(query_.get(), L"\\Thermal Zone Information(*)\\Temperature",
                                  0, &counter_) != ERROR_SUCCESS) {
            counter_ = nullptr;
            return;
        }
        // Values are not valid until the second collect, which the first sample() does.
        PdhCollectQueryData(query_.get());
    }

    float read_pdh() {
        if (!query_ || !counter_) return -1;
        if (PdhCollectQueryData(query_.get()) != ERROR_SUCCESS) return -1;

        DWORD bytes = 0, items = 0;
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &items, nullptr)
                != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bytes == 0)
            return -1;

        buf_.resize((bytes + sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) - 1)
                    / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &items, buf_.data())
                != ERROR_SUCCESS)
            return -1;

        float best = -1;
        for (DWORD i = 0; i < items; ++i) {
            const auto& v = buf_[i].FmtValue;
            if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            // PDH reports thermal zones in Kelvin.
            const float c = static_cast<float>(v.doubleValue - 273.15);
            if (c > best) best = c;
        }
        return best;
    }

    pawnio::Module module_;
    bool amd_ = false;
    float tdie_offset_ = 0;
    float tjmax_ = 100;
    UniqueHandle pci_mutex_;

    PdhQuery query_;
    PDH_HCOUNTER counter_ = nullptr;
    // Reused across samples; PDH writes the name strings into the same buffer after the items.
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buf_;
};

} // namespace

std::unique_ptr<Sampler> make_thermal_sampler() { return std::make_unique<ThermalSampler>(); }

} // namespace ring0
