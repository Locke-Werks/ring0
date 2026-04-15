#include "netif.h"
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

/* Per-luid delta cache. Modest overprovision so hotplugs don't evict real
   adapters; 32 slots covers physical NICs, VPN taps, and vEthernet ports
   without breaking a sweat. */
typedef struct {
    uint64_t luid;
    uint64_t prev_in;
    uint64_t prev_out;
    uint64_t prev_ms;
} slot_t;

#define SLOT_N (NETIF_MAX * 4)
static slot_t g_slots[SLOT_N];

static slot_t *find_slot(uint64_t luid)
{
    slot_t *empty = NULL;
    for (int i = 0; i < SLOT_N; ++i) {
        if (g_slots[i].luid == luid) return &g_slots[i];
        if (g_slots[i].luid == 0 && !empty) empty = &g_slots[i];
    }
    return empty;
}

int netif_sample(net_if_t *out, int max, uint64_t now_ms)
{
    PMIB_IF_TABLE2 tbl = NULL;
    if (GetIfTable2(&tbl) != NO_ERROR || !tbl) return 0;

    int n = 0;
    for (ULONG i = 0; i < tbl->NumEntries && n < max; ++i) {
        MIB_IF_ROW2 *row = &tbl->Table[i];
        if (row->Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (!row->InterfaceAndOperStatusFlags.HardwareInterface) continue;

        net_if_t *o = &out[n];
        int k = 0;
        for (; k < 63 && row->Alias[k]; ++k) o->name[k] = row->Alias[k];
        o->name[k] = 0;

        o->in_bytes   = row->InOctets;
        o->out_bytes  = row->OutOctets;
        o->link_speed = row->ReceiveLinkSpeed < row->TransmitLinkSpeed
                        ? row->ReceiveLinkSpeed : row->TransmitLinkSpeed;
        o->up         = (row->MediaConnectState == MediaConnectStateConnected) ? 1 : 0;
        o->in_bps     = 0;
        o->out_bps    = 0;
        o->_pad       = 0;

        slot_t *s = find_slot(row->InterfaceLuid.Value);
        if (s) {
            if (s->luid != 0 && now_ms > s->prev_ms) {
                uint64_t dwall = now_ms - s->prev_ms;
                if (o->in_bytes >= s->prev_in) {
                    uint64_t bps = (o->in_bytes - s->prev_in) * 1000 / dwall;
                    o->in_bps = bps > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)bps;
                }
                if (o->out_bytes >= s->prev_out) {
                    uint64_t bps = (o->out_bytes - s->prev_out) * 1000 / dwall;
                    o->out_bps = bps > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)bps;
                }
            }
            s->luid     = row->InterfaceLuid.Value;
            s->prev_in  = o->in_bytes;
            s->prev_out = o->out_bytes;
            s->prev_ms  = now_ms;
        }

        ++n;
    }
    FreeMibTable(tbl);
    return n;
}
