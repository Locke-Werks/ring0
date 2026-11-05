#pragma once
#include <stdint.h>

#define NETIF_MAX 8

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    wchar_t  name[64];       /* MIB_IF_ROW2.Alias */
    uint64_t in_bytes;
    uint64_t out_bytes;
    uint32_t in_bps;
    uint32_t out_bps;
    uint64_t link_speed;     /* bits/sec, min(tx,rx) */
    uint32_t up;             /* MediaConnectStateConnected */
    uint32_t _pad;
} net_if_t;

/* Walks GetIfTable2, filters out loopback and non-hardware interfaces, fills
   `out` with up to `max` entries. `now_ms` drives per-iface byte-rate deltas. */
int netif_sample(net_if_t *out, int max, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
