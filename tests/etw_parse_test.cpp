// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/etw_parse.hpp"

#include <cstdio>
#include <cstring>

using namespace ring0::etw;

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

template <class T>
static void put(unsigned char* buf, std::size_t at, T v) {
    std::memcpy(buf + at, &v, sizeof v);
}

int main() {
    // x64 DiskIo_TypeGroup1 payload is 52 bytes, IssuingThreadId last at 48.
    CHECK(disk_issuing_tid_offset(8) == 48);
    CHECK(disk_issuing_tid_offset(4) == 40);

    unsigned char disk[52] = {};
    put<std::uint32_t>(disk, 8, 1u << 20);
    put<std::uint32_t>(disk, 48, 0x1234);
    CHECK(disk_transfer_size(disk, sizeof disk) == (1u << 20));
    CHECK(disk_issuing_tid(disk, sizeof disk, 8) == 0x1234u);
    // A truncated payload yields nothing rather than reading past the end.
    CHECK(!disk_issuing_tid(disk, 50, 8));
    CHECK(!disk_transfer_size(disk, 8));

    unsigned char thread[8] = {};
    put<std::uint32_t>(thread, 0, 4242);
    put<std::uint32_t>(thread, 4, 0x1234);
    const auto ids = thread_ids(thread, sizeof thread);
    CHECK(ids && ids->pid == 4242 && ids->tid == 0x1234);
    CHECK(!thread_ids(thread, 6));

    if (failures == 0) std::puts("etw_parse: ok");
    return failures == 0 ? 0 : 1;
}
