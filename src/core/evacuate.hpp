// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

// Soft-disables logical processors by steering every process off them. The kernel
// keeps scheduling interrupts, DPCs and its own threads there, and protected
// processes refuse the change, so this empties a core of ordinary work rather than
// taking it offline.
//
// Each process is moved with CPU Sets, a scheduler preference that leaves the
// affinity mask untouched, so software that inspects its affinity sees nothing
// unusual. Affinity masks are the fallback where CPU Sets are refused, and only on
// single-group machines, since a mask cannot express a set that spans groups.
//
// Indices are logical processors in group-major order, matching the CPU sampler.

namespace ring0 {

class CoreEvacuator {
public:
    struct Stats {
        int moved = 0;     // processes currently steered off the excluded cores
        int refused = 0;   // processes that could not be changed (protected, or pinned to excluded cores only)
    };

    CoreEvacuator();
    ~CoreEvacuator();

    CoreEvacuator(const CoreEvacuator&) = delete;
    CoreEvacuator& operator=(const CoreEvacuator&) = delete;

    int core_count() const noexcept { return static_cast<int>(lps_.size()); }

    // Replaces the excluded set and reapplies it to every process. An exclusion that
    // would leave no core allowed is rejected and returns false with nothing changed:
    // an empty CPU set means "no preference" and a zero mask is invalid, so neither
    // could express it.
    bool set_excluded(const std::vector<bool>& excluded);
    const std::vector<bool>& excluded() const noexcept { return excluded_; }
    bool any_excluded() const noexcept;

    // Applies the current exclusion to processes started since the last pass and
    // forgets processes that have exited. Cheap when nothing is new.
    void sweep();

    // Puts every changed process back the way it was found.
    void restore_all();

    Stats stats() const noexcept;

private:
    enum class Method : std::uint8_t { CpuSets, Affinity, Refused };

    struct Record {
        std::uint64_t created = 0;              // guards against pid reuse
        Method method = Method::Refused;
        std::vector<unsigned long> orig_sets;   // empty means the process had no CPU set preference
        std::uintptr_t orig_mask = 0;
    };

    struct Lp {
        unsigned long cpu_set_id = 0;
        std::uint16_t group = 0;
        std::uint8_t index = 0;                 // within its group
    };

    void apply(std::uint32_t pid, Record* existing);
    void restore(std::uint32_t pid, const Record& r);

    std::vector<Lp> lps_;
    bool single_group_ = true;
    std::vector<bool> excluded_;
    std::unordered_map<std::uint32_t, Record> records_;
};

} // namespace ring0
