// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sampler.hpp"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <memory>
#include <type_traits>
#include <vector>

#pragma comment(lib, "pdh.lib")

namespace ring0 {
namespace {

struct PdhQueryCloser {
    void operator()(PDH_HQUERY q) const noexcept { PdhCloseQuery(q); }
};
using PdhQuery = std::unique_ptr<std::remove_pointer_t<PDH_HQUERY>, PdhQueryCloser>;

class ThermalSampler final : public Sampler {
public:
    ThermalSampler() {
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

    void sample(Snapshot& out, const Snapshot*, double) override {
        out.cpu_temp_c = -1;
        if (!query_ || !counter_) return;
        if (PdhCollectQueryData(query_.get()) != ERROR_SUCCESS) return;

        DWORD bytes = 0, items = 0;
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &items, nullptr)
                != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bytes == 0)
            return;

        buf_.resize((bytes + sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) - 1)
                    / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
        if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &items, buf_.data())
                != ERROR_SUCCESS)
            return;

        float best = -1;
        for (DWORD i = 0; i < items; ++i) {
            const auto& v = buf_[i].FmtValue;
            if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            // PDH reports thermal zones in Kelvin.
            const float c = static_cast<float>(v.doubleValue - 273.15);
            if (c > best) best = c;
        }
        out.cpu_temp_c = best;
    }

private:
    PdhQuery query_;
    PDH_HCOUNTER counter_ = nullptr;
    // Reused across samples; PDH writes the name strings into the same buffer after the items.
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buf_;
};

} // namespace

std::unique_ptr<Sampler> make_thermal_sampler() { return std::make_unique<ThermalSampler>(); }

} // namespace ring0
