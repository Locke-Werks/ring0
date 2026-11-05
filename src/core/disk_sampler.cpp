// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sampler.hpp"

#include <windows.h>
#include <winioctl.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ring0 {
namespace {

// Disk numbers are dense in practice; 32 covers hot-plugged USB and VHD mounts.
constexpr int kMaxPhysicalDrives = 32;

struct HandleCloser {
    HANDLE h = INVALID_HANDLE_VALUE;
    HandleCloser() = default;
    explicit HandleCloser(HANDLE v) noexcept : h(v) {}
    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;
    HandleCloser(HandleCloser&& o) noexcept : h(std::exchange(o.h, INVALID_HANDLE_VALUE)) {}
    HandleCloser& operator=(HandleCloser&& o) noexcept {
        if (this != &o) { reset(); h = std::exchange(o.h, INVALID_HANDLE_VALUE); }
        return *this;
    }
    ~HandleCloser() { reset(); }
    void reset() noexcept {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] bool valid() const noexcept { return h != INVALID_HANDLE_VALUE && h != nullptr; }
};

// Drives update their sensors slowly and the query can wake a sleeping link.
constexpr std::uint64_t kTempIntervalMs   = 5000;
// Mounts change rarely; the enumeration opens every volume, so keep it off the hot path.
constexpr std::uint64_t kVolumeIntervalMs = 10000;

struct FindVolumeCloser {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit FindVolumeCloser(HANDLE v) noexcept : h(v) {}
    FindVolumeCloser(const FindVolumeCloser&) = delete;
    FindVolumeCloser& operator=(const FindVolumeCloser&) = delete;
    ~FindVolumeCloser() { if (h != INVALID_HANDLE_VALUE) FindVolumeClose(h); }
};

struct Slot {
    bool          tried = false;
    HandleCloser  handle;
    HandleCloser  read_handle;    // only when elevated; some IOCTLs demand read access
    std::wstring  model;
    std::uint64_t capacity = 0;
    float         temp_c = -1.0f;
    bool          temp_unsupported = false;
    std::uint64_t temp_ms = 0;
    bool          temp_queried = false;
    bool          have_prev = false;
    std::uint64_t prev_read = 0, prev_write = 0;
    std::uint64_t prev_ms = 0;
};

struct VolumeInfo {
    std::wstring  letters;
    std::uint64_t free_bytes = 0;
};

std::wstring drive_name(int n) { return std::format(L"PhysicalDrive{}", n); }

// Access mask 0 is the metadata-only idiom: no elevation needed, and the
// performance and storage-property IOCTLs both accept it.
HandleCloser open_disk(int n, DWORD access = 0) {
    const std::wstring path = L"\\\\.\\" + drive_name(n);
    return HandleCloser{CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr)};
}

std::uint64_t query_capacity(HANDLE meta, HANDLE rd) {
    // Geometry is FILE_ANY_ACCESS, so it works on the metadata handle unelevated.
    DISK_GEOMETRY_EX geo{};
    DWORD ret = 0;
    if (DeviceIoControl(meta, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                        &geo, sizeof geo, &ret, nullptr) && geo.DiskSize.QuadPart > 0)
        return static_cast<std::uint64_t>(geo.DiskSize.QuadPart);
    if (rd == INVALID_HANDLE_VALUE) return 0;
    GET_LENGTH_INFORMATION len{};
    if (DeviceIoControl(rd, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                        &len, sizeof len, &ret, nullptr) && len.Length.QuadPart > 0)
        return static_cast<std::uint64_t>(len.Length.QuadPart);
    return 0;
}

std::optional<float> query_temp_property(HANDLE h) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceTemperatureProperty;
    q.QueryType  = PropertyStandardQuery;
    alignas(STORAGE_TEMPERATURE_DATA_DESCRIPTOR) std::array<std::byte, 1024> buf{};
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q,
                         buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr))
        return std::nullopt;
    constexpr DWORD header = offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo);
    if (ret < header + sizeof(STORAGE_TEMPERATURE_INFO)) return std::nullopt;
    const auto* d = reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(buf.data());
    const std::size_t fit = (ret - header) / sizeof(STORAGE_TEMPERATURE_INFO);
    const std::size_t n = std::min<std::size_t>(d->InfoCount, fit);
    if (n == 0) return std::nullopt;
    // Index 0 is the composite sensor when the drive reports one; otherwise the first entry.
    const STORAGE_TEMPERATURE_INFO* pick = &d->TemperatureInfo[0];
    for (std::size_t i = 0; i < n; ++i)
        if (d->TemperatureInfo[i].Index == 0) { pick = &d->TemperatureInfo[i]; break; }
    const SHORT t = pick->Temperature;
    // Some bridges report 0 or absurd values when no sensor is wired through.
    if (t <= 0 || t > 150) return std::nullopt;
    return static_cast<float>(t);
}

std::optional<float> query_temp_nvme_log(HANDLE h) {
    constexpr ULONG kHealthLogId = 0x02;  // NVMe SMART / Health Information log page
    constexpr ULONG kLogLen = 512;        // the docs require at least 512 for log pages
    constexpr DWORD dataStart = offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters)
                                + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    alignas(8) std::array<std::byte, dataStart + kLogLen> buf{};

    auto* q = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(buf.data());
    q->PropertyId = StorageDeviceProtocolSpecificProperty;
    q->QueryType  = PropertyStandardQuery;
    auto* p = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(q->AdditionalParameters);
    p->ProtocolType             = ProtocolTypeNvme;
    p->DataType                 = NVMeDataTypeLogPage;
    p->ProtocolDataRequestValue = kHealthLogId;
    p->ProtocolDataOffset       = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    p->ProtocolDataLength       = kLogLen;

    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf.data(), static_cast<DWORD>(buf.size()),
                         buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr))
        return std::nullopt;
    const auto* desc = reinterpret_cast<const STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(buf.data());
    if (desc->Version != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) ||
        desc->Size != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR))
        return std::nullopt;
    const auto& out = desc->ProtocolSpecificData;
    const std::size_t base = offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
    if (out.ProtocolDataOffset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) || out.ProtocolDataLength < 3)
        return std::nullopt;
    const std::size_t at = base + out.ProtocolDataOffset;
    if (at + 3 > buf.size() || at + 3 > ret) return std::nullopt;
    // Byte 0 is the critical warning; bytes 1..2 are composite temperature in Kelvin.
    const unsigned kelvin = static_cast<unsigned>(buf[at + 1]) |
                            (static_cast<unsigned>(buf[at + 2]) << 8);
    if (kelvin <= 273 || kelvin > 273 + 150) return std::nullopt;
    return static_cast<float>(static_cast<int>(kelvin) - 273);
}

void refresh_temp(Slot& s, std::uint64_t now_ms) {
    if (s.temp_unsupported) return;
    if (s.temp_queried && now_ms - s.temp_ms < kTempIntervalMs) return;
    s.temp_queried = true;
    s.temp_ms = now_ms;
    const HANDLE meta = s.handle.h;
    const HANDLE rd = s.read_handle.valid() ? s.read_handle.h : INVALID_HANDLE_VALUE;
    std::optional<float> t = query_temp_property(meta);
    if (!t && rd != INVALID_HANDLE_VALUE) t = query_temp_property(rd);
    if (!t) t = query_temp_nvme_log(meta);
    if (!t && rd != INVALID_HANDLE_VALUE) t = query_temp_nvme_log(rd);
    if (t) { s.temp_c = *t; return; }
    s.temp_c = -1.0f;
    // USB bridges and virtual disks never answer; stop paying for the attempt.
    if (GetLastError() == ERROR_INVALID_FUNCTION || GetLastError() == ERROR_NOT_SUPPORTED)
        s.temp_unsupported = true;
}

// Maps disk number to the volumes it hosts. Spanned and mirrored volumes
// appear on every disk they touch.
std::array<VolumeInfo, kMaxPhysicalDrives> map_volumes() {
    std::array<VolumeInfo, kMaxPhysicalDrives> map{};
    std::array<wchar_t, MAX_PATH + 1> name{};
    FindVolumeCloser find{FindFirstVolumeW(name.data(), static_cast<DWORD>(name.size()))};
    if (find.h == INVALID_HANDLE_VALUE) return map;
    do {
        std::wstring vol(name.data());
        if (vol.empty() || vol.back() != L'\\') continue;

        // CreateFileW wants the volume device without the trailing backslash.
        const std::wstring dev = vol.substr(0, vol.size() - 1);
        HandleCloser h{CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr)};
        if (!h.valid()) continue;

        std::vector<std::byte> ext(sizeof(VOLUME_DISK_EXTENTS));
        DWORD ret = 0;
        bool ok = DeviceIoControl(h.h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                                  ext.data(), static_cast<DWORD>(ext.size()), &ret, nullptr) != 0;
        if (!ok && GetLastError() == ERROR_MORE_DATA) {
            const auto* head = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(ext.data());
            const std::size_t need = offsetof(VOLUME_DISK_EXTENTS, Extents)
                                     + std::size_t(head->NumberOfDiskExtents) * sizeof(DISK_EXTENT);
            ext.assign(need, std::byte{});
            ok = DeviceIoControl(h.h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                                 ext.data(), static_cast<DWORD>(ext.size()), &ret, nullptr) != 0;
        }
        if (!ok) continue;
        const auto* vde = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(ext.data());

        ULARGE_INTEGER free_avail{}, total{}, free_total{};
        const std::uint64_t free_b = GetDiskFreeSpaceExW(vol.c_str(), &free_avail, &total, &free_total)
                                         ? free_total.QuadPart : 0;

        std::wstring letters;
        DWORD need = 0;
        GetVolumePathNamesForVolumeNameW(vol.c_str(), nullptr, 0, &need);
        if (need > 0) {
            std::vector<wchar_t> paths(need);
            if (GetVolumePathNamesForVolumeNameW(vol.c_str(), paths.data(), need, &need)) {
                for (const wchar_t* p = paths.data(); *p; p += wcslen(p) + 1) {
                    // Folder mount points are not drive letters.
                    if (wcslen(p) == 3 && p[1] == L':') {
                        if (!letters.empty()) letters += L' ';
                        letters += p[0];
                        letters += L':';
                    }
                }
            }
        }

        std::array<bool, kMaxPhysicalDrives> seen{};
        for (DWORD e = 0; e < vde->NumberOfDiskExtents; ++e) {
            const DWORD n = vde->Extents[e].DiskNumber;
            if (n >= DWORD(kMaxPhysicalDrives) || seen[n]) continue;
            seen[n] = true;
            VolumeInfo& vi = map[n];
            vi.free_bytes += free_b;
            if (!letters.empty()) {
                if (!vi.letters.empty()) vi.letters += L' ';
                vi.letters += letters;
            }
        }
    } while (FindNextVolumeW(find.h, name.data(), static_cast<DWORD>(name.size())));
    return map;
}

std::wstring trim(std::wstring s) {
    const auto first = s.find_first_not_of(L' ');
    if (first == std::wstring::npos) return {};
    const auto last = s.find_last_not_of(L' ');
    return s.substr(first, last - first + 1);
}

std::wstring query_model(HANDLE h) {
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;

    std::array<std::byte, 1024> buf{};
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q,
                         buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr))
        return {};
    if (ret < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return {};

    const auto* d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
    const auto ascii_at = [&](DWORD off) -> std::string {
        if (off == 0 || off >= ret) return {};
        const char* p = reinterpret_cast<const char*>(buf.data()) + off;
        const std::size_t max = ret - off;
        std::size_t len = 0;
        while (len < max && p[len] != '\0') ++len;
        return std::string(p, len);
    };

    std::string vendor  = ascii_at(d->VendorIdOffset);
    std::string product = ascii_at(d->ProductIdOffset);
    std::string joined;
    // NVMe drives often report a bogus vendor; product alone already carries the brand.
    if (!vendor.empty() && vendor.find_first_not_of(' ') != std::string::npos)
        joined = vendor + " ";
    joined += product;

    std::wstring out;
    out.reserve(joined.size());
    for (unsigned char c : joined) out.push_back(static_cast<wchar_t>(c));
    return trim(std::move(out));
}

float rate(std::uint64_t now, std::uint64_t then, double secs) {
    if (now < then || secs <= 0) return 0.0f;
    return static_cast<float>(static_cast<double>(now - then) / secs);
}

// Active time comes from the PhysicalDisk "% Idle Time" counter, the source
// Task Manager uses. DISK_PERFORMANCE.IdleTime is only updated when a disk goes
// idle, so a delta of it cannot measure an interval that was busy.
class ActiveTime {
public:
    ActiveTime()
    {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) { query_ = nullptr; return; }
        if (PdhAddEnglishCounterW(query_, L"\\PhysicalDisk(*)\\% Idle Time", 0, &counter_) != ERROR_SUCCESS) {
            PdhCloseQuery(query_);
            query_ = nullptr;
            return;
        }
        PdhCollectQueryData(query_);  // rate counters need a first sample to diff against
    }
    ~ActiveTime() { if (query_) PdhCloseQuery(query_); }
    ActiveTime(const ActiveTime&) = delete;
    ActiveTime& operator=(const ActiveTime&) = delete;

    // Busy percent per disk number, -1 where the counter has no instance.
    std::array<float, 32> collect()
    {
        std::array<float, 32> busy;
        busy.fill(-1.0f);
        if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return busy;
        DWORD bytes = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr) != PDH_MORE_DATA)
            return busy;
        std::vector<std::byte> buf(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) != ERROR_SUCCESS)
            return busy;
        for (DWORD i = 0; i < count; ++i) {
            // Instances are "<disk number> <volumes>", e.g. "1 C: D:"; "_Total" is skipped.
            const wchar_t* name = items[i].szName;
            if (!name || name[0] < L'0' || name[0] > L'9') continue;
            const int n = _wtoi(name);
            if (n < 0 || n >= 32 || items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
            busy[std::size_t(n)] = float(std::clamp(100.0 - items[i].FmtValue.doubleValue, 0.0, 100.0));
        }
        return busy;
    }

private:
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
};

class DiskSampler final : public Sampler {
public:
    void sample(Snapshot& out, const Snapshot* prev, double dt) override {
        try {
            sample_impl(out, prev, dt);
        } catch (...) {
            // Allocation failure is the only realistic source; a partial frame beats a dead sampler.
        }
    }

private:
    void sample_impl(Snapshot& out, const Snapshot*, double) {
        const auto busy = active_.collect();
        out.disks.clear();
        out.disk_bps_total = 0;
        const std::uint64_t now_ms = GetTickCount64();

        if (!volumes_valid_ || now_ms - volumes_ms_ >= kVolumeIntervalMs) {
            volumes_ = map_volumes();
            volumes_ms_ = now_ms;
            volumes_valid_ = true;
        }

        for (int i = 0; i < kMaxPhysicalDrives; ++i) {
            Slot& s = slots_[static_cast<std::size_t>(i)];
            if (!s.tried) {
                s.tried  = true;
                s.handle = open_disk(i);
                if (s.handle.valid()) {
                    s.model = query_model(s.handle.h);
                    // Fails without elevation, which leaves the metadata handle to do what it can.
                    s.read_handle = open_disk(i, GENERIC_READ);
                    if (!s.read_handle.valid()) s.read_handle = open_disk(i, FILE_READ_ATTRIBUTES);
                    s.capacity = query_capacity(s.handle.h,
                                                s.read_handle.valid() ? s.read_handle.h : INVALID_HANDLE_VALUE);
                }
            }
            if (!s.handle.valid()) continue;

            DISK_PERFORMANCE dp{};
            DWORD ret = 0;
            if (!DeviceIoControl(s.handle.h, IOCTL_DISK_PERFORMANCE, nullptr, 0,
                                 &dp, sizeof dp, &ret, nullptr)) {
                // A removed drive fails here forever; drop it so a new disk at
                // the same number gets opened fresh on the next pass.
                s = Slot{};
                continue;
            }

            Disk d;
            d.name  = drive_name(i);
            d.model = s.model;
            d.capacity = s.capacity;
            refresh_temp(s, now_ms);
            d.temp_c = s.temp_c;
            d.volumes = volumes_[std::size_t(i)].letters;
            d.free_bytes = volumes_[std::size_t(i)].free_bytes;

            const auto rd = static_cast<std::uint64_t>(dp.BytesRead.QuadPart);
            const auto wr = static_cast<std::uint64_t>(dp.BytesWritten.QuadPart);

            if (s.have_prev && now_ms > s.prev_ms) {
                const double secs = static_cast<double>(now_ms - s.prev_ms) / 1000.0;
                d.read_bps  = rate(rd, s.prev_read, secs);
                d.write_bps = rate(wr, s.prev_write, secs);
                if (busy[std::size_t(i)] >= 0) d.active_pct = busy[std::size_t(i)];
            }

            s.have_prev  = true;
            s.prev_read  = rd;
            s.prev_write = wr;
            s.prev_ms    = now_ms;

            out.disk_bps_total += d.read_bps + d.write_bps;
            out.disks.push_back(std::move(d));
        }
    }

    std::array<Slot, kMaxPhysicalDrives> slots_{};
    std::array<VolumeInfo, kMaxPhysicalDrives> volumes_{};
    std::uint64_t volumes_ms_ = 0;
    bool volumes_valid_ = false;
    ActiveTime active_;
};

} // namespace

std::unique_ptr<Sampler> make_disk_sampler() { return std::make_unique<DiskSampler>(); }

} // namespace ring0
