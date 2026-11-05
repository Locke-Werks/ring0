// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

// Payload readers for the kernel logger's classic events, kept free of ETW types so
// they can be tested on synthetic buffers.

namespace ring0::etw {

inline std::optional<std::uint32_t> read_u32(const void* data, std::size_t len, std::size_t at) {
    if (!data || at + sizeof(std::uint32_t) > len) return std::nullopt;
    std::uint32_t v;
    std::memcpy(&v, static_cast<const unsigned char*>(data) + at, sizeof v);
    return v;
}

// DiskIo_TypeGroup1: DiskNumber, IrpFlags, TransferSize, Reserved (4 x u32),
// ByteOffset (u64), FileObject, Irp (pointers), HighResResponseTime (u64),
// IssuingThreadId (u32). Disk completions are logged from a DPC, so the event
// header's process id is usually -1; the issuing thread is what names the process.
inline std::size_t disk_issuing_tid_offset(std::size_t pointer_size) {
    return 4 * sizeof(std::uint32_t) + 8 + 2 * pointer_size + 8;
}

inline std::optional<std::uint32_t> disk_transfer_size(const void* data, std::size_t len) {
    return read_u32(data, len, 2 * sizeof(std::uint32_t));
}

inline std::optional<std::uint32_t> disk_issuing_tid(const void* data, std::size_t len,
                                                     std::size_t pointer_size) {
    return read_u32(data, len, disk_issuing_tid_offset(pointer_size));
}

// Thread_TypeGroup1 leads with ProcessId then TThreadId.
struct ThreadIds {
    std::uint32_t pid;
    std::uint32_t tid;
};

inline std::optional<ThreadIds> thread_ids(const void* data, std::size_t len) {
    const auto pid = read_u32(data, len, 0);
    const auto tid = read_u32(data, len, sizeof(std::uint32_t));
    if (!pid || !tid) return std::nullopt;
    return ThreadIds{*pid, *tid};
}

} // namespace ring0::etw
