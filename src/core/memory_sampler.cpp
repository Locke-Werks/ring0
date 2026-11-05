// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sampler.hpp"
#include "core/pawnio.hpp"

#include <windows.h>
#include <psapi.h>
#include <sddl.h>
#include <winternl.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace ring0 {
namespace {

// DDR5 module temperatures from each DIMM's SPD5118 hub (JESD300-5), reached over the
// chipset SMBus through the PawnIO SmbusPIIX4 / SmbusI801 modules. Register map and
// conversion follow Linux drivers/hwmon/spd5118.c; controller and port handling follow
// RAMSPDToolkit, which LibreHardwareMonitor uses for the same job.
class DimmThermal {
public:
    // Fills out with the cached readings, refreshing them at most every kInterval.
    void sample(std::vector<float>& out) noexcept {
        try {
            if (!initialized_) {
                initialized_ = true;
                init();
            }
            if (hubs_.empty()) return;
            const auto now = std::chrono::steady_clock::now();
            if (!have_read_ || now - last_read_ >= kInterval) {
                last_read_ = now;
                have_read_ = true;
                refresh();
            }
            out = cache_;
        } catch (...) {
            out.clear();
        }
    }

    ~DimmThermal() {
        if (mutex_) CloseHandle(mutex_);
    }

private:
    static constexpr auto kInterval = std::chrono::seconds(5);

    // Linux i2c_smbus_xfer encoding, which both modules take as in[3].
    static constexpr std::uint64_t kByteData = 2;
    static constexpr std::uint64_t kWordData = 3;

    static constexpr std::uint8_t kRegType = 0x00;      // MR0:MR1, reads 0x51 0x18
    static constexpr std::uint8_t kRegCap = 0x05;       // MR5, bit 1 = sensor present
    static constexpr std::uint8_t kRegLegacy = 0x0B;    // MR11, bits 2:0 = NVM page
    static constexpr std::uint8_t kRegTempCfg = 0x1A;   // MR26, bit 0 = sensor disabled
    static constexpr std::uint8_t kRegTemp = 0x31;      // MR49:MR50

    // PIIX4 port 0 is where every AM4/AM5 board seen so far puts the DIMMs; port 1 is the
    // aux controller with its own base address. -1 means the module has no port concept.
    static constexpr int kNoPort = -1;

    struct Hub {
        int port;
        std::uint8_t addr;
    };

    pawnio::Module module_;
    bool piix4_ = false;
    HANDLE mutex_ = nullptr;
    std::vector<Hub> hubs_;
    std::vector<float> cache_;
    std::chrono::steady_clock::time_point last_read_{};
    bool initialized_ = false;
    bool have_read_ = false;

    static std::filesystem::path module_path(const wchar_t* file) noexcept {
        std::array<wchar_t, MAX_PATH> exe{};
        const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (n == 0 || n >= exe.size()) return {};
        try {
            return std::filesystem::path(exe.data()).parent_path() / L"pawnio" / file;
        } catch (...) {
            return {};
        }
    }

    // The name every SMBus tool on Windows agrees on (HWiNFO, LHM/RAMSPDToolkit, the
    // PawnIO module docs). The world-access DACL matters: whichever process creates it
    // first decides who else can open it, and a default DACL from an elevated process
    // would lock out a non-elevated tool and break the exclusion.
    static HANDLE open_smbus_mutex() noexcept {
        constexpr const wchar_t* kName = L"Global\\Access_SMBUS.HTP.Method";
        PSECURITY_DESCRIPTOR sd = nullptr;
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1,
                                                                 &sd, nullptr))
            sa.lpSecurityDescriptor = sd;
        HANDLE h = CreateMutexW(&sa, FALSE, kName);
        if (sd) LocalFree(sd);
        if (!h && GetLastError() == ERROR_ACCESS_DENIED)
            h = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, kName);
        return h;
    }

    class Lock {
    public:
        explicit Lock(HANDLE m) noexcept : m_(m) {
            // A short wait: another tool mid-transaction finishes in milliseconds, and a
            // stuck holder should cost one skipped refresh, not a stalled sampler thread.
            const DWORD r = m_ ? WaitForSingleObject(m_, 200) : WAIT_FAILED;
            held_ = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
        }
        ~Lock() {
            if (held_) ReleaseMutex(m_);
        }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
        explicit operator bool() const noexcept { return held_; }

    private:
        HANDLE m_;
        bool held_ = false;
    };

    std::optional<std::uint16_t> read(std::uint8_t addr, std::uint8_t reg,
                                      std::uint64_t proto) const noexcept {
        const std::array<std::uint64_t, 4> in{addr, 1, reg, proto};
        std::array<std::uint64_t, 1> out{};
        auto r = module_.execute("ioctl_smbus_xfer", in, out);
        if (!r || *r < 1) return std::nullopt;
        return static_cast<std::uint16_t>(out[0] & (proto == kWordData ? 0xFFFF : 0xFF));
    }

    bool write_byte(std::uint8_t addr, std::uint8_t reg, std::uint8_t value) const noexcept {
        const std::array<std::uint64_t, 5> in{addr, 0, reg, kByteData, value};
        return module_.execute("ioctl_smbus_xfer", in, {}).has_value();
    }

    // Returns the previous port so the caller can put the controller back the way other
    // tools expect it. kNoPort on failure or on controllers without ports.
    std::optional<int> select_port(int port) const noexcept {
        if (!piix4_ || port == kNoPort) return kNoPort;
        auto r = module_.call1("ioctl_piix4_port_sel", static_cast<std::uint64_t>(port));
        if (!r) return std::nullopt;
        return static_cast<int>(static_cast<std::int64_t>(*r));
    }

    void restore_port(int old_port) const noexcept {
        if (piix4_ && old_port != kNoPort)
            (void)module_.call1("ioctl_piix4_port_sel", static_cast<std::uint64_t>(old_port));
    }

    static std::uint16_t swap16(std::uint16_t v) noexcept {
        return static_cast<std::uint16_t>((v << 8) | (v >> 8));
    }

    bool is_spd5118(std::uint8_t addr) const noexcept {
        auto type = read(addr, kRegType, kWordData);
        if (!type) return false;
        if (swap16(*type) != 0x5118) {
            // Some hubs read MR0:MR1 as zero while MR11 points at a non-zero NVM page
            // (noted in the Linux driver). Pointing it back at page 0 is what every other
            // SPD tool does before reading too, so it does not disturb them.
            if (*type != 0) return false;
            auto legacy = read(addr, kRegLegacy, kByteData);
            if (!legacy || (*legacy & 0x07) == 0) return false;
            if (!write_byte(addr, kRegLegacy, static_cast<std::uint8_t>(*legacy & ~0x07u)))
                return false;
            type = read(addr, kRegType, kWordData);
            if (!type || swap16(*type) != 0x5118) return false;
        }
        auto cap = read(addr, kRegCap, kByteData);
        if (!cap || (*cap & 0x02) == 0) return false;
        auto cfg = read(addr, kRegTempCfg, kByteData);
        return cfg && (*cfg & 0x01) == 0;
    }

    void probe(int port) {
        Lock lock(mutex_);
        if (!lock) return;
        auto old = select_port(port);
        if (!old) return;
        for (std::uint8_t a = 0x50; a <= 0x57; ++a)
            if (is_spd5118(a)) hubs_.push_back({port, a});
        restore_port(*old);
    }

    void init() {
        if (!pawnio::available()) return;
        mutex_ = open_smbus_mutex();
        if (!mutex_) return;

        // Each module's entry point refuses to load when its controller is absent, so the
        // load itself is the controller detection.
        if (auto m = pawnio::Module::load_file(module_path(L"SmbusPIIX4.bin"))) {
            module_ = std::move(*m);
            piix4_ = true;
            probe(0);
            if (hubs_.empty()) probe(1);
        }
        if (hubs_.empty()) {
            piix4_ = false;
            module_.close();
            if (auto m = pawnio::Module::load_file(module_path(L"SmbusI801.bin"))) {
                module_ = std::move(*m);
                probe(kNoPort);
            }
        }
        if (hubs_.empty()) module_.close();
    }

    // MR49:MR50 holds an 11-bit two's-complement value in bits 12:2, 0.25 C per step.
    static float decode(std::uint16_t raw) noexcept {
        int v = (raw >> 2) & 0x7FF;
        if (v & 0x400) v -= 0x800;
        return static_cast<float>(v) * 0.25f;
    }

    void refresh() {
        std::vector<float> temps;
        {
            Lock lock(mutex_);
            if (!lock) return;
            int current = kNoPort - 1;
            std::optional<int> old;
            for (const Hub& h : hubs_) {
                if (h.port != current) {
                    if (old) restore_port(*old);
                    old = select_port(h.port);
                    current = h.port;
                    if (!old) continue;
                }
                if (!old) continue;
                // MR49 sits below 0x80, outside the paged NVM window, so no MR11 write
                // is needed per read.
                auto raw = read(h.addr, kRegTemp, kWordData);
                if (!raw) continue;
                const float c = decode(*raw);
                if (c >= -20.0f && c <= 125.0f) temps.push_back(c);
            }
            if (old) restore_port(*old);
        }
        cache_ = std::move(temps);
    }
};

constexpr ULONG kSystemPerformanceInformation = 2;
constexpr ULONG kSystemMemoryListInformation = 80;

// The memory manager's own page lists. Unlike GetPerformanceInfo's SystemCache
// (standby plus the system working set, which overlaps "in use"), these partition
// physical memory, so a composition bar built from them never double counts.
struct SystemMemoryListInformation {
    ULONG_PTR ZeroPageCount;
    ULONG_PTR FreePageCount;
    ULONG_PTR ModifiedPageCount;
    ULONG_PTR ModifiedNoWritePageCount;
    ULONG_PTR BadPageCount;
    ULONG_PTR PageCountByPriority[8];
    ULONG_PTR RepurposedPagesByPriority[8];
    ULONG_PTR ModifiedPageCountPageFile;
};

// The list query is gated on SeProfileSingleProcessPrivilege, which an elevated
// token holds but leaves disabled.
bool enable_profile_privilege()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = LookupPrivilegeValueW(nullptr, L"SeProfileSingleProcessPrivilege", &tp.Privileges[0].Luid)
                    && AdjustTokenPrivileges(token, FALSE, &tp, sizeof tp, nullptr, nullptr)
                    && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

// Only the prefix up to NonPagedPoolPages is read; the layout of that prefix has been
// stable since XP. The tail pads the buffer so newer kernels that write a longer struct
// do not fail with STATUS_INFO_LENGTH_MISMATCH.
struct SystemPerformanceInformation {
    LARGE_INTEGER IdleProcessTime;
    LARGE_INTEGER IoReadTransferCount;
    LARGE_INTEGER IoWriteTransferCount;
    LARGE_INTEGER IoOtherTransferCount;
    ULONG IoReadOperationCount;
    ULONG IoWriteOperationCount;
    ULONG IoOtherOperationCount;
    ULONG AvailablePages;
    ULONG CommittedPages;
    ULONG CommitLimit;
    ULONG PeakCommitment;
    ULONG PageFaultCount;
    ULONG CopyOnWriteCount;
    ULONG TransitionCount;
    ULONG CacheTransitionCount;
    ULONG DemandZeroCount;
    ULONG PageReadCount;
    ULONG PageReadIoCount;
    ULONG CacheReadCount;
    ULONG CacheIoCount;
    ULONG DirtyPagesWriteCount;
    ULONG DirtyWriteIoCount;
    ULONG MappedPagesWriteCount;
    ULONG MappedWriteIoCount;
    ULONG PagedPoolPages;
    ULONG NonPagedPoolPages;
    ULONG PagedPoolAllocs;
    ULONG PagedPoolFrees;
    ULONG NonPagedPoolAllocs;
    ULONG NonPagedPoolFrees;
    ULONG FreeSystemPtes;
    BYTE  tail[512];
};

class MemorySampler final : public Sampler {
public:
    void sample(Snapshot& out, const Snapshot* prev, double dt_seconds) override {
        Memory& m = out.memory;
        dimms_.sample(m.dimm_temps_c);

        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms)) {
            m.phys_total   = ms.ullTotalPhys;
            m.phys_avail   = ms.ullAvailPhys;
            m.commit_limit = ms.ullTotalPageFile;
            m.commit_used  = ms.ullTotalPageFile > ms.ullAvailPageFile
                                 ? ms.ullTotalPageFile - ms.ullAvailPageFile : 0;
        }

        PERFORMANCE_INFORMATION pi{};
        pi.cb = sizeof pi;
        std::uint64_t page = 0;
        if (GetPerformanceInfo(&pi, sizeof pi)) {
            page = pi.PageSize;
            // SystemCache is standby plus the system working set; kept only as a
            // fallback when the page lists below are unavailable.
            m.cached = static_cast<std::uint64_t>(pi.SystemCache) * page;
        }
        if (page == 0) {
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            page = si.dwPageSize;
        }

        if (!privilege_tried_) { privilege_tried_ = true; enable_profile_privilege(); }
        SystemMemoryListInformation ml{};
        ULONG got = 0;
        if (NT_SUCCESS(NtQuerySystemInformation(
                static_cast<SYSTEM_INFORMATION_CLASS>(kSystemMemoryListInformation), &ml, sizeof ml, &got))) {
            std::uint64_t standby = 0;
            for (ULONG_PTR c : ml.PageCountByPriority) standby += c;
            m.standby   = standby * page;
            m.modified  = (static_cast<std::uint64_t>(ml.ModifiedPageCount) + ml.ModifiedNoWritePageCount) * page;
            m.free_list = (static_cast<std::uint64_t>(ml.FreePageCount) + ml.ZeroPageCount) * page;
            // Task Manager's "Cached" is standby plus modified; match it.
            m.cached    = m.standby + m.modified;
        }

        SystemPerformanceInformation spi{};
        ULONG ret = 0;
        if (!NT_SUCCESS(NtQuerySystemInformation(
                static_cast<SYSTEM_INFORMATION_CLASS>(kSystemPerformanceInformation),
                &spi, sizeof spi, &ret)))
            return;

        m.paged_pool    = static_cast<std::uint64_t>(spi.PagedPoolPages) * page;
        m.nonpaged_pool = static_cast<std::uint64_t>(spi.NonPagedPoolPages) * page;

        // The counter is a 32-bit ULONG that wraps; unsigned subtraction handles that.
        if (have_prev_ && prev && dt_seconds > 0) {
            const std::uint32_t delta = spi.PageFaultCount - prev_faults_;
            m.page_faults_per_sec = static_cast<float>(delta / dt_seconds);
        }
        prev_faults_ = spi.PageFaultCount;
        have_prev_ = true;
    }

private:
    DimmThermal dimms_;
    std::uint32_t prev_faults_ = 0;
    bool privilege_tried_ = false;
    bool have_prev_ = false;
};

} // namespace

std::unique_ptr<Sampler> make_memory_sampler() { return std::make_unique<MemorySampler>(); }

} // namespace ring0
