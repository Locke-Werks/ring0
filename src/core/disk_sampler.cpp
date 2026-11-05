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
#include <format>
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

struct Slot {
    bool          tried = false;
    HandleCloser  handle;
    std::wstring  model;
    bool          have_prev = false;
    std::uint64_t prev_read = 0, prev_write = 0;
    std::uint64_t prev_ms = 0;
};

std::wstring drive_name(int n) { return std::format(L"PhysicalDrive{}", n); }

// Access mask 0 is the metadata-only idiom: no elevation needed, and the
// performance and storage-property IOCTLs both accept it.
HandleCloser open_disk(int n) {
    const std::wstring path = L"\\\\.\\" + drive_name(n);
    return HandleCloser{CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr)};
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
    void sample(Snapshot& out, const Snapshot*, double) override {
        const auto busy = active_.collect();
        out.disks.clear();
        out.disk_bps_total = 0;
        const std::uint64_t now_ms = GetTickCount64();

        for (int i = 0; i < kMaxPhysicalDrives; ++i) {
            Slot& s = slots_[static_cast<std::size_t>(i)];
            if (!s.tried) {
                s.tried  = true;
                s.handle = open_disk(i);
                if (s.handle.valid()) s.model = query_model(s.handle.h);
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

private:
    std::array<Slot, kMaxPhysicalDrives> slots_{};
    ActiveTime active_;
};

} // namespace

std::unique_ptr<Sampler> make_disk_sampler() { return std::make_unique<DiskSampler>(); }

} // namespace ring0
