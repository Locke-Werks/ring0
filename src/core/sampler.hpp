// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/snapshot.hpp"

// Each subsystem fills its slice of a Snapshot. Samplers keep whatever previous-sample
// state they need for rates; `prev` is the last published snapshot, null on the first pass.
// Samplers run only on the sampler thread and must never throw; on failure they leave
// their slice empty.

namespace ring0 {

class Sampler {
public:
    virtual ~Sampler() = default;
    virtual void sample(Snapshot& out, const Snapshot* prev, double dt_seconds) = 0;
};

std::unique_ptr<Sampler> make_process_sampler();
std::unique_ptr<Sampler> make_cpu_sampler();
std::unique_ptr<Sampler> make_memory_sampler();
std::unique_ptr<Sampler> make_disk_sampler();
std::unique_ptr<Sampler> make_net_sampler();
std::unique_ptr<Sampler> make_gpu_sampler();      // also fills Process::gpu_100ns/gpu_pct; runs after process
std::unique_ptr<Sampler> make_thermal_sampler();
std::unique_ptr<Sampler> make_etw_sampler();      // owns the ETW session; folds totals into Process
std::unique_ptr<Sampler> make_inference_sampler(); // reads gpu_dedicated/gpu_shared; runs after gpu

} // namespace ring0
