// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The contract between the sampler thread and everything that reads from it.
// A Snapshot is immutable once published; readers hold a shared_ptr and never lock.

namespace ring0 {

inline constexpr std::size_t kMaxCores   = 256;
inline constexpr std::size_t kMaxEngines = 16;

struct Process {
    std::uint32_t pid = 0;
    std::uint32_t ppid = 0;
    std::uint32_t session_id = 0;
    std::uint32_t thread_count = 0;
    std::uint32_t handle_count = 0;
    std::uint8_t  base_priority = 0;
    bool          suspended = false;     // every thread in a suspended wait
    bool          is_system = false;     // session 0 or a protected/system image
    std::wstring  image_name;
    std::wstring  image_path;            // full DOS path; empty if access was denied
    std::wstring  command_line;          // empty if not resolved
    std::uint64_t create_time = 0;       // 100ns since 1601
    std::uint64_t working_set = 0;       // bytes
    std::uint64_t private_bytes = 0;
    std::uint64_t cpu_100ns = 0;         // kernel + user, cumulative
    std::uint64_t gpu_100ns = 0;         // cumulative across adapters and engines
    std::uint64_t io_read_bytes = 0;
    std::uint64_t io_write_bytes = 0;
    std::uint64_t etw_disk_bytes = 0;    // cumulative, from the ETW DiskIo provider
    std::uint64_t etw_net_bytes = 0;     // cumulative, from the ETW TcpIp provider
    // Rates computed against the previous snapshot by the sampler thread.
    float cpu_pct = 0;                   // 0..100 of the whole machine
    float gpu_pct = 0;
    float disk_bps = 0;
    float net_bps = 0;
};

struct CoreLoad {
    float total = 0;   // 0..100
    float user = 0;
    float kernel = 0;  // excludes idle and DPC/interrupt
};

struct Memory {
    std::uint64_t phys_total = 0, phys_avail = 0;
    std::uint64_t commit_limit = 0, commit_used = 0;
    std::uint64_t paged_pool = 0, nonpaged_pool = 0;
    std::uint64_t cached = 0;
    // Memory manager page lists (bytes), from SystemMemoryListInformation. These
    // partition physical RAM without overlap; 0 when the query is unavailable.
    std::uint64_t standby = 0;    // reclaimable cache, all eight priorities
    std::uint64_t modified = 0;   // dirty, waiting to be written; counted as in use
    std::uint64_t free_list = 0;  // free + zeroed
    float page_faults_per_sec = 0;
    std::vector<float> dimm_temps_c;  // per populated DDR5 module (SPD hub sensor); empty if unavailable
};

struct Disk {
    std::wstring name;            // "PhysicalDrive0"
    std::wstring model;
    float read_bps = 0, write_bps = 0;
    float active_pct = 0;
    float temp_c = -1;            // drive-reported temperature, -1 if unavailable
    std::wstring volumes;         // mounted drive letters, e.g. "C: D:"
    std::uint64_t capacity = 0;   // bytes, whole device
    std::uint64_t free_bytes = 0; // bytes free across its mounted volumes
};

struct NetIf {
    std::wstring name;
    std::uint64_t link_bps = 0;
    float rx_bps = 0, tx_bps = 0;
};

struct GpuEngine {
    std::wstring name;            // "3D", "Copy", "VideoDecode", ...
    float pct = 0;
};

struct Gpu {
    std::wstring name;
    std::uint64_t dedicated_total = 0, dedicated_used = 0;
    std::uint64_t shared_total = 0, shared_used = 0;
    float temp_c = -1;
    float total_pct = 0;          // busiest engine, matching Task Manager
    std::vector<GpuEngine> engines;
};

struct Snapshot {
    std::uint64_t seq = 0;
    std::uint64_t tick_ms = 0;    // GetTickCount64 at sample time
    double time_ms = 0;           // timebase::now_ms() at sample start; graphs position by this
    std::vector<Process> processes;
    std::vector<CoreLoad> cores;
    float cpu_total = 0;
    float cpu_temp_c = -1;        // package / Tdie; -1 when nothing reports
    std::vector<float> ccd_temps_c;   // per chiplet (AMD) or per core (Intel); empty if unavailable
    float cpu_power_w = -1;           // package power from the energy counter; -1 if unavailable
    std::vector<float> core_mhz;      // effective clock per logical core; empty if unavailable
    Memory memory;
    std::vector<Disk> disks;
    std::vector<NetIf> ifaces;
    std::vector<Gpu> gpus;
    // Machine roll-ups for the scrubber strip.
    float disk_bps_total = 0;
    float net_bps_total = 0;
    float gpu_total = 0;
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

} // namespace ring0
