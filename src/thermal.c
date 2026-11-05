#include "thermal.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#define ZONE_MAX 8

static PDH_HQUERY    g_query;
static PDH_HCOUNTER  g_counter;
static float         g_temps[ZONE_MAX];
static int           g_n_zones;
static int           g_ready;

void thermal_init(void)
{
    if (g_ready) return;
    g_ready = 1;

    if (PdhOpenQueryW(NULL, 0, &g_query) != ERROR_SUCCESS) {
        g_query = NULL;
        return;
    }

    /* wildcard counter picks up all ACPI thermal zones. on most machines
       zone 0 is the cpu package; others may be chipset, nvme, etc. */
    PDH_STATUS st = PdhAddEnglishCounterW(g_query,
        L"\\Thermal Zone Information(*)\\Temperature",
        0, &g_counter);
    if (st != ERROR_SUCCESS) {
        g_counter = NULL;
        return;
    }
    /* first collect primes the counters; values aren't valid until the
       second collect, which happens on the next thermal_sample(). */
    PdhCollectQueryData(g_query);
}

void thermal_destroy(void)
{
    if (g_query) PdhCloseQuery(g_query);
    g_query   = NULL;
    g_counter = NULL;
    g_n_zones = 0;
    g_ready   = 0;
}

void thermal_sample(void)
{
    if (!g_query || !g_counter) return;
    PdhCollectQueryData(g_query);

    DWORD buf_sz = 0, n_items = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(g_counter, PDH_FMT_DOUBLE,
                                                  &buf_sz, &n_items, NULL);
    if (st != PDH_MORE_DATA || buf_sz == 0) { g_n_zones = 0; return; }

    PDH_FMT_COUNTERVALUE_ITEM_W *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)
        HeapAlloc(GetProcessHeap(), 0, buf_sz);
    if (!items) return;

    st = PdhGetFormattedCounterArrayW(g_counter, PDH_FMT_DOUBLE,
                                      &buf_sz, &n_items, items);
    if (st == ERROR_SUCCESS) {
        g_n_zones = (int)(n_items > ZONE_MAX ? ZONE_MAX : n_items);
        for (int i = 0; i < g_n_zones; ++i) {
            /* PDH reports thermal zones in Kelvin */
            g_temps[i] = (float)(items[i].FmtValue.doubleValue - 273.15);
        }
    } else {
        g_n_zones = 0;
    }
    HeapFree(GetProcessHeap(), 0, items);
}

float thermal_cpu_temp(void)
{
    /* return the hottest zone — zone 0 is usually the cpu package; on
       multi-zone systems the highest reading is the safest bet. */
    float best = -1.0f;
    for (int i = 0; i < g_n_zones; ++i)
        if (g_temps[i] > best) best = g_temps[i];
    return best;
}
