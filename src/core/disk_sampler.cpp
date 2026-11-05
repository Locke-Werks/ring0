// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sampler.hpp"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

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
    std::int64_t  prev_idle = 0, prev_query = 0;
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

class DiskSampler final : public Sampler {
public:
    void sample(Snapshot& out, const Snapshot*, double) override {
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
            const std::int64_t idle  = dp.IdleTime.QuadPart;
            const std::int64_t query = dp.QueryTime.QuadPart;

            if (s.have_prev && now_ms > s.prev_ms) {
                const double secs = static_cast<double>(now_ms - s.prev_ms) / 1000.0;
                d.read_bps  = rate(rd, s.prev_read, secs);
                d.write_bps = rate(wr, s.prev_write, secs);
                const std::int64_t dq = query - s.prev_query;
                const std::int64_t di = idle - s.prev_idle;
                if (dq > 0 && di >= 0) {
                    const double busy = 1.0 - static_cast<double>(di) / static_cast<double>(dq);
                    d.active_pct = static_cast<float>(std::clamp(busy, 0.0, 1.0) * 100.0);
                }
            }

            s.have_prev  = true;
            s.prev_read  = rd;
            s.prev_write = wr;
            s.prev_idle  = idle;
            s.prev_query = query;
            s.prev_ms    = now_ms;

            out.disk_bps_total += d.read_bps + d.write_bps;
            out.disks.push_back(std::move(d));
        }
    }

private:
    std::array<Slot, kMaxPhysicalDrives> slots_{};
};

} // namespace

std::unique_ptr<Sampler> make_disk_sampler() { return std::make_unique<DiskSampler>(); }

} // namespace ring0
