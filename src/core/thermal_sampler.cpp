// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sampler.hpp"
#include "core/pawnio.hpp"

#include <windows.h>
#include <intrin.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

#pragma comment(lib, "pdh.lib")

namespace ring0 {
namespace {

// Register layouts follow LibreHardwareMonitor's Amd17Cpu.cs and IntelCpu.cs, which in turn
// follow Linux k10temp and the Intel SDM. MSR numbers are the ones the PawnIO modules
// allow-list; anything else comes back STATUS_ACCESS_DENIED.
constexpr std::uint64_t kThmTconCurTmp = 0x00059800; // AMD SMN, THM_TCON_CUR_TMP
constexpr std::uint32_t kTempRangeSel = 0x00080000; // bit 19
constexpr std::uint32_t kTempTjSelMask = 0x00030000; // bits 17:16

constexpr std::uint64_t kF17M70CcdTemp = 0x00059954; // Zen 2 / Zen 3, eight slots
constexpr std::uint64_t kF17M61CcdTemp = 0x00059B08; // Raphael / Granite Ridge, eight slots
constexpr std::uint64_t kF1AM00CcdTemp = 0x000599F0; // Turin / Shimada Peak, sixteen slots
constexpr std::uint32_t kCcdTempValid = 0x800;

constexpr std::uint64_t kAmdPwrUnit = 0xC0010299;
constexpr std::uint64_t kAmdPkgEnergyStat = 0xC001029B;
// The read-only aliases; AMDFamily17 does not allow-list the architectural 0xE8.
constexpr std::uint64_t kAmdAperfRo = 0xC00000E8;

constexpr std::uint64_t kIa32PackageThermStatus = 0x1B1;
constexpr std::uint64_t kIa32ThermStatus = 0x19C;
constexpr std::uint64_t kIa32TemperatureTarget = 0x1A2;
constexpr std::uint64_t kIa32Aperf = 0xE8;
constexpr std::uint64_t kRaplPowerUnit = 0x606;
constexpr std::uint64_t kPkgEnergyStatus = 0x611;

// No shipping part sustains this; a larger delta means a counter was reset under us.
constexpr double kMaxPlausibleMhz = 20000.0;

enum class Vendor { Other, Amd, Intel };

struct CpuInfo {
    Vendor vendor = Vendor::Other;
    unsigned family = 0;
    unsigned model = 0;
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
    const auto eax = static_cast<unsigned>(r[0]);
    const unsigned base = (eax >> 8) & 0xF;
    const unsigned ext = (eax >> 20) & 0xFF;
    info.family = base == 0xF ? base + ext : base;
    info.model = (eax >> 4) & 0xF;
    if (base == 0xF || base == 0x6) info.model |= ((eax >> 16) & 0xF) << 4;

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

struct CcdLayout {
    std::uint64_t base = 0;
    unsigned slots = 0;
    bool valid_bit = false; // Turin flags presence explicitly; older parts read zero when absent
};

// Mirrors LHM's model switch: per-CCD sensors are only read on models it has seen report
// them, because on others the same SMN offsets hold unrelated registers.
CcdLayout amd_ccd_layout(const CpuInfo& cpu) noexcept {
    if (cpu.family == 0x1A && cpu.model <= 0x2F) return {kF1AM00CcdTemp, 16, true};
    switch (cpu.model) {
    case 0x61: case 0x44: return {kF17M61CcdTemp, 8, false};
    case 0x31: case 0x71: case 0x21: return {kF17M70CcdTemp, 8, false};
    default: return {};
    }
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

// Logical processors in group-major, bit-ascending order, which is the order the CPU
// sampler gets from SystemProcessorPerformanceInformation, so core_mhz[i] lines up with cores[i].
std::vector<GROUP_AFFINITY> logical_processors() {
    std::vector<GROUP_AFFINITY> out;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationGroup, nullptr, &bytes);
    if (bytes == 0) return out;
    std::vector<std::byte> buf(bytes);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationGroup, info, &bytes)) return out;
    for (WORD g = 0; g < info->Group.ActiveGroupCount; ++g) {
        const KAFFINITY mask = info->Group.GroupInfo[g].ActiveProcessorMask;
        for (unsigned b = 0; b < sizeof(KAFFINITY) * 8; ++b) {
            if ((mask >> b) & 1) {
                GROUP_AFFINITY a{};
                a.Group = g;
                a.Mask = KAFFINITY{1} << b;
                out.push_back(a);
            }
        }
    }
    return out;
}

// One affinity per physical core, its lowest logical processor. Intel's IA32_THERM_STATUS
// is per core and shared by its SMT siblings, so reading it once per core is enough.
std::vector<GROUP_AFFINITY> physical_cores() {
    std::vector<GROUP_AFFINITY> out;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (bytes == 0) return out;
    std::vector<std::byte> buf(bytes);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()), &bytes))
        return out;
    for (DWORD off = 0; off < bytes;) {
        const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (e->Size == 0) break;
        if (e->Processor.GroupCount >= 1) {
            const GROUP_AFFINITY& gm = e->Processor.GroupMask[0];
            if (gm.Mask != 0) {
                GROUP_AFFINITY a{};
                a.Group = gm.Group;
                unsigned long bit = 0;
#if defined(_WIN64)
                _BitScanForward64(&bit, gm.Mask);
#else
                _BitScanForward(&bit, gm.Mask);
#endif
                a.Mask = KAFFINITY{1} << bit;
                out.push_back(a);
            }
        }
        off += e->Size;
    }
    return out;
}

// The module functions run msr_read on whatever CPU the ioctl arrives on, which is the
// calling thread's CPU, and none of them pins internally. Per-core MSRs therefore need
// this thread moved onto the target core first. The guard puts the sampler thread back
// on its original affinity and priority however the pass ends.
//
// Priority is raised for the pass because migrating onto a core that is saturated with
// normal-priority work otherwise waits for that core's quantum to expire: up to roughly
// 20 ms per core, which at 32 logical processors would stretch one pass toward a second.
class PinGuard {
public:
    PinGuard() noexcept {
        thread_ = GetCurrentThread();
        saved_ok_ = GetThreadGroupAffinity(thread_, &saved_) != FALSE;
        prio_ = GetThreadPriority(thread_);
        if (prio_ != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(thread_, THREAD_PRIORITY_HIGHEST);
    }
    PinGuard(const PinGuard&) = delete;
    PinGuard& operator=(const PinGuard&) = delete;
    ~PinGuard() {
        if (saved_ok_) SetThreadGroupAffinity(thread_, &saved_, nullptr);
        if (prio_ != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(thread_, prio_);
    }

    bool ok() const noexcept { return saved_ok_; }

    // On return the scheduler has already moved the thread, since the new mask excludes
    // every other processor.
    bool pin(const GROUP_AFFINITY& a) const noexcept {
        return SetThreadGroupAffinity(thread_, &a, nullptr) != FALSE;
    }

private:
    HANDLE thread_ = nullptr;
    GROUP_AFFINITY saved_{};
    bool saved_ok_ = false;
    int prio_ = THREAD_PRIORITY_ERROR_RETURN;
};

// RAPL energy counters are 32 bits wide and wrap. Unsigned subtraction recovers the delta
// across one wrap; two wraps cannot happen between samples, since at 15.3 uJ per count
// the counter spans about 65.5 kJ, around 280 s at a 230 W package limit.
float energy_to_watts(std::uint32_t now, std::uint32_t prev, double joules_per_count,
                      double dt_seconds) noexcept {
    if (dt_seconds <= 0) return -1;
    const std::uint32_t d = now - prev;
    const double w = static_cast<double>(d) * joules_per_count / dt_seconds;
    return std::isfinite(w) ? static_cast<float>(w) : -1.0f;
}

class ThermalSampler final : public Sampler {
public:
    ThermalSampler() {
        cpu_ = identify();
        if (cpu_.vendor == Vendor::Amd && cpu_.family >= 0x17 && cpu_.family <= 0x1A)
            init_amd();
        else if (cpu_.vendor == Vendor::Intel && cpu_.intel_pkg_sensor)
            init_intel();

        if (!module_) init_pdh();
    }

    void sample(Snapshot& out, const Snapshot*, double dt_seconds) override {
        out.cpu_temp_c = -1;
        out.ccd_temps_c.clear();
        out.cpu_power_w = -1;
        out.core_mhz.clear();

        if (!module_) {
            out.cpu_temp_c = read_pdh();
            return;
        }

        if (amd_) {
            read_amd_smn(out);
        } else {
            out.cpu_temp_c = read_intel_package();
        }
        read_power(out, dt_seconds);
        try {
            per_core_pass(out, dt_seconds);
        } catch (...) {
            out.ccd_temps_c.clear();
            out.core_mhz.clear();
        }
    }

private:
    void init_amd() {
        auto m = pawnio::Module::load_file(module_path(L"AMDFamily17.bin"));
        if (!m) return;
        module_ = std::move(*m);
        amd_ = true;
        tdie_offset_ = amd_tdie_offset(cpu_.brand.data());
        ccd_ = amd_ccd_layout(cpu_);
        aperf_msr_ = kAmdAperfRo;
        // SMN goes through the PCI config index/data pair at 0:0.0 offsets 0x60/0x64. Other
        // monitors share that pair, and the module says to hold this mutex so an interleaved
        // index write from another process cannot redirect our data read.
        pci_mutex_.reset(CreateMutexW(nullptr, FALSE, L"Global\\Access_PCI"));
        if (!pci_mutex_) pci_mutex_.reset(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                                     L"Global\\Access_PCI"));
        // ESU, bits 12:8: one count is 1/2^ESU joule (15.3 uJ at the reset value of 16).
        if (auto u = module_.call1("ioctl_read_msr", kAmdPwrUnit))
            joules_per_count_ = std::ldexp(1.0, -static_cast<int>((*u >> 8) & 0x1F));
        init_cores();
    }

    void init_intel() {
        auto m = pawnio::Module::load_file(module_path(L"IntelMSR.bin"));
        if (!m) return;
        module_ = std::move(*m);
        aperf_msr_ = kIa32Aperf;
        // TjMax is fused and does not change at runtime; 100 C is the documented fallback LHM uses.
        if (auto r = module_.call1("ioctl_read_msr", kIa32TemperatureTarget)) {
            const unsigned tj = static_cast<unsigned>(*r >> 16) & 0xFF;
            if (tj != 0) tjmax_ = static_cast<float>(tj);
        }
        // Same ESU field layout as AMD. Silvermont and Airmont scale it differently; LHM
        // special-cases them by microarchitecture, which this sampler does not decode, so
        // those Atom parts would read wrong and are not worth a family/model table here.
        if (auto u = module_.call1("ioctl_read_msr", kRaplPowerUnit))
            joules_per_count_ = std::ldexp(1.0, -static_cast<int>((*u >> 8) & 0x1F));
        init_cores();
    }

    void init_cores() {
        try {
            cpus_ = logical_processors();
            if (!amd_) phys_ = physical_cores();
        } catch (...) {
            cpus_.clear();
            phys_.clear();
        }
        prev_aperf_.assign(cpus_.size(), std::nullopt);

        // Hybrid Intel parts can fuse a different TjMax per core type, so it is read per
        // core once here, falling back to the package value.
        if (!phys_.empty()) {
            core_tjmax_.assign(phys_.size(), tjmax_);
            PinGuard guard;
            if (!guard.ok()) return;
            for (std::size_t i = 0; i < phys_.size(); ++i) {
                if (!guard.pin(phys_[i])) continue;
                if (auto r = module_.call1("ioctl_read_msr", kIa32TemperatureTarget)) {
                    const unsigned tj = static_cast<unsigned>(*r >> 16) & 0xFF;
                    if (tj != 0) core_tjmax_[i] = static_cast<float>(tj);
                }
            }
        }
    }

    // Tctl and the CCD sensors share one hold on the PCI mutex: they are all package-wide
    // SMN registers, readable from any core, so this part needs no pinning at all.
    void read_amd_smn(Snapshot& out) {
        if (pci_mutex_) {
            const DWORD w = WaitForSingleObject(pci_mutex_.get(), 10);
            if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return;
        }

        if (auto r = module_.call1("ioctl_read_smn", kThmTconCurTmp)) {
            const auto raw = static_cast<std::uint32_t>(*r);
            // CUR_TEMP is bits 31:21 in 0.125 C steps. With range select set the scale starts at
            // -49 C so the sensor can report below zero; newer parts flag the same via TJ_SEL.
            float t = static_cast<float>(raw >> 21) * 0.125f;
            if ((raw & kTempRangeSel) != 0 || (raw & kTempTjSelMask) == kTempTjSelMask) t -= 49.0f;
            out.cpu_temp_c = t + tdie_offset_;
        }

        // Slots for absent CCDs read zero (or lack the valid bit on Turin), which is how
        // LHM and k10temp tell a 7950X's two populated CCDs from the six empty slots.
        for (unsigned i = 0; i < ccd_.slots; ++i) {
            auto r = module_.call1("ioctl_read_smn", ccd_.base + i * 4ull);
            if (!r) break;
            const auto raw = static_cast<std::uint32_t>(*r);
            const bool present = ccd_.valid_bit ? (raw & kCcdTempValid) != 0 : (raw & 0xFFF) != 0;
            if (!present) continue;
            // Bits 11:0 in 0.125 C steps, offset by -305 C as in k10temp's CCD reading.
            const float t = static_cast<float>(raw & 0xFFF) * 0.125f - 305.0f;
            if (t < 125.0f) out.ccd_temps_c.push_back(t);
        }

        if (pci_mutex_) ReleaseMutex(pci_mutex_.get());
    }

    float read_intel_package() noexcept {
        auto r = module_.call1("ioctl_read_msr", kIa32PackageThermStatus);
        if (!r) return -1;
        const auto v = static_cast<std::uint32_t>(*r);
        if ((v & 0x80000000u) == 0) return -1; // reading-valid bit
        // Bits 22:16 are degrees below TjMax, not an absolute temperature.
        return tjmax_ - static_cast<float>((v >> 16) & 0x7F);
    }

    // Package energy is a package-scope MSR, so any core answers with the same counter.
    void read_power(Snapshot& out, double dt_seconds) {
        if (joules_per_count_ <= 0) return;
        auto r = module_.call1("ioctl_read_msr", amd_ ? kAmdPkgEnergyStat : kPkgEnergyStatus);
        if (!r) {
            prev_energy_.reset();
            return;
        }
        const auto now = static_cast<std::uint32_t>(*r);
        if (prev_energy_) out.cpu_power_w = energy_to_watts(now, *prev_energy_, joules_per_count_, dt_seconds);
        prev_energy_ = now;
    }

    // Cost, per sample on the 7950X: 32 migrations plus 32 APERF ioctls. An ioctl into
    // the PawnIO interpreter is a syscall and a few hundred bytecode ops, single-digit
    // microseconds. A migration is a reschedule onto another core, on the order of 5 to
    // 20 us with the raised priority. That puts the pass near 32 * 25 us = 0.8 ms, about
    // 0.1% of one core at a 1 s interval, so clocks are read every sample. MPERF is not
    // read: effective clock needs only APERF over wall time, and skipping it halves the
    // ioctls. CCD temperatures stay out of this loop entirely since SMN is package-wide.
    void per_core_pass(Snapshot& out, double dt_seconds) {
        if (cpus_.empty() && phys_.empty()) return;
        PinGuard guard;
        if (!guard.ok()) return;

        // Intel per-core temperature reuses the same kind of pinning, one read per core.
        if (!phys_.empty()) {
            out.ccd_temps_c.reserve(phys_.size());
            for (std::size_t i = 0; i < phys_.size(); ++i) {
                float t = -1;
                if (guard.pin(phys_[i])) {
                    if (auto r = module_.call1("ioctl_read_msr", kIa32ThermStatus)) {
                        const auto v = static_cast<std::uint32_t>(*r);
                        if ((v & 0x80000000u) != 0)
                            t = core_tjmax_[i] - static_cast<float>((v >> 16) & 0x7F);
                    }
                }
                out.ccd_temps_c.push_back(t);
            }
        }

        if (cpus_.empty()) return;
        bool any = false;
        std::vector<float> mhz(cpus_.size(), -1.0f);
        for (std::size_t i = 0; i < cpus_.size(); ++i) {
            std::optional<std::uint64_t> now;
            if (guard.pin(cpus_[i])) {
                if (auto r = module_.call1("ioctl_read_msr", aperf_msr_)) now = *r;
            }
            auto& prev = prev_aperf_[i];
            // APERF advances at the actual clock only while the core is in C0, so its rate
            // over wall time is the effective clock, idle time included. A counter that went
            // backwards was reset or wrapped; 64 bits at 6 GHz cannot wrap in practice, so
            // either way the sample is dropped rather than guessed.
            if (now && prev && *now >= *prev && dt_seconds > 0) {
                const double f = static_cast<double>(*now - *prev) / dt_seconds / 1e6;
                if (f <= kMaxPlausibleMhz) {
                    mhz[i] = static_cast<float>(f);
                    any = true;
                }
            }
            prev = now;
        }
        if (any) out.core_mhz = std::move(mhz);
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

    CpuInfo cpu_;
    pawnio::Module module_;
    bool amd_ = false;
    float tdie_offset_ = 0;
    float tjmax_ = 100;
    UniqueHandle pci_mutex_;
    CcdLayout ccd_;

    double joules_per_count_ = 0;
    std::optional<std::uint32_t> prev_energy_;

    std::uint64_t aperf_msr_ = 0;
    std::vector<GROUP_AFFINITY> cpus_;
    std::vector<GROUP_AFFINITY> phys_;
    std::vector<float> core_tjmax_;
    std::vector<std::optional<std::uint64_t>> prev_aperf_;

    PdhQuery query_;
    PDH_HCOUNTER counter_ = nullptr;
    // Reused across samples; PDH writes the name strings into the same buffer after the items.
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buf_;
};

} // namespace

std::unique_ptr<Sampler> make_thermal_sampler() { return std::make_unique<ThermalSampler>(); }

} // namespace ring0
