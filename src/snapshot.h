#pragma once
#include <stdint.h>
#include "arena.h"

typedef struct {
    uint32_t pid;
    uint32_t ppid;
    uint32_t image_name;      /* intern handle */
    uint32_t session_id;
    uint32_t thread_count;
    uint32_t handle_count;
    uint32_t page_faults;
    uint32_t _pad;
    uint64_t working_set;     /* bytes */
    uint64_t virtual_size;    /* bytes */
    uint64_t cpu_100ns;       /* kernel + user time, 100ns units */
    uint64_t io_read_bytes;
    uint64_t io_write_bytes;
    uint64_t io_other_bytes;
    uint64_t create_time;     /* 100ns since 1601 */
} process_t;

typedef struct {
    uint64_t   timestamp;     /* GetTickCount64 at sample */
    uint32_t   n_processes;
    uint32_t   _pad;
    process_t *processes;     /* flat array in the same arena */
} snapshot_t;

snapshot_t *snapshot_build(arena_t *dest, arena_t *scratch);
