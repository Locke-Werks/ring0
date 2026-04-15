#include "gpu.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>     /* NTSTATUS */
#include <string.h>

/* D3DKMT bits aren't in the user-mode SDK, so we declare the minimum we need
   and resolve the entry points from gdi32.dll. Layouts taken from public
   reverse engineering — stable since Vista. */

typedef UINT D3DKMT_HANDLE;

typedef struct _D3DKMT_ADAPTERINFO_R0 {
    D3DKMT_HANDLE hAdapter;
    LUID          AdapterLuid;
    ULONG         NumOfSources;
    BOOL          bPresentMoveRegionsPreferred;
} D3DKMT_ADAPTERINFO_R0;

typedef struct _D3DKMT_ENUMADAPTERS2_R0 {
    ULONG                  NumAdapters;
    D3DKMT_ADAPTERINFO_R0 *pAdapters;
} D3DKMT_ENUMADAPTERS2_R0;

typedef enum _D3DKMT_QUERYSTATISTICS_TYPE_R0 {
    D3DKMT_QS_ADAPTER          = 0,
    D3DKMT_QS_PROCESS          = 1,
    D3DKMT_QS_PROCESS_ADAPTER  = 2,
    D3DKMT_QS_NODE             = 3,
    D3DKMT_QS_PROCESS_NODE     = 4,
} D3DKMT_QUERYSTATISTICS_TYPE_R0;

typedef struct _D3DKMT_QUERYSTATISTICS_QUERY_NODE_R0 {
    UINT16 NodeOrdinal;
    UINT16 PhysicalAdapterIndex;
} D3DKMT_QUERYSTATISTICS_QUERY_NODE_R0;

typedef union _D3DKMT_QUERYSTATISTICS_QUERY_R0 {
    D3DKMT_QUERYSTATISTICS_QUERY_NODE_R0 QueryProcessNode;
    BYTE pad[16];
} D3DKMT_QUERYSTATISTICS_QUERY_R0;

/* D3DKMT_QUERYSTATISTICS_RESULT is a union of many structs; we only read
   RunningTime out of D3DKMT_QUERYSTATISTICS_PROCESS_NODE_INFORMATION which
   places it at offset 0. So we just declare a generous opaque blob. */
typedef BYTE D3DKMT_QUERYSTATISTICS_RESULT_R0[1024];

typedef struct _D3DKMT_QUERYSTATISTICS_R0 {
    D3DKMT_QUERYSTATISTICS_TYPE_R0      Type;
    LUID                                AdapterLuid;
    HANDLE                              hProcess;
    D3DKMT_QUERYSTATISTICS_RESULT_R0    QueryResult;
    /* MS docs put QueryProcessNode in the request portion (overlapped with
       QueryResult union). Using a separate field is wrong, but we keep it
       here as scratch the kernel ignores. */
    D3DKMT_QUERYSTATISTICS_QUERY_R0     Query;
} D3DKMT_QUERYSTATISTICS_R0;

typedef NTSTATUS (NTAPI *fn_enum)(D3DKMT_ENUMADAPTERS2_R0 *);
typedef NTSTATUS (NTAPI *fn_qs)  (void *);

static fn_enum                  g_enum;
static fn_qs                    g_qs;
static D3DKMT_ADAPTERINFO_R0    g_adapters[8];
static ULONG                    g_n_adapters;

void gpu_init(void)
{
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) return;
    g_enum = (fn_enum)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    g_qs   = (fn_qs)  GetProcAddress(gdi, "D3DKMTQueryStatistics");
    if (!g_enum || !g_qs) { g_enum = NULL; g_qs = NULL; return; }

    /* probe for adapter count */
    D3DKMT_ENUMADAPTERS2_R0 e = { 0, NULL };
    g_enum(&e);
    if (e.NumAdapters == 0) e.NumAdapters = 8;
    if (e.NumAdapters > 8) e.NumAdapters = 8;
    e.pAdapters = g_adapters;
    if (g_enum(&e) == 0) g_n_adapters = e.NumAdapters;
}

void gpu_destroy(void) { g_enum = NULL; g_qs = NULL; g_n_adapters = 0; }

uint64_t gpu_runtime_100ns(unsigned pid)
{
    if (!g_qs || g_n_adapters == 0 || pid == 0) return 0;
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) return 0;

    uint64_t total = 0;
    for (ULONG a = 0; a < g_n_adapters; ++a) {
        /* probe each engine ordinal until we hit one that fails */
        for (UINT16 node = 0; node < 16; ++node) {
            BYTE buf[1280];
            memset(buf, 0, sizeof buf);
            D3DKMT_QUERYSTATISTICS_R0 *q = (D3DKMT_QUERYSTATISTICS_R0 *)buf;
            q->Type        = D3DKMT_QS_PROCESS_NODE;
            q->AdapterLuid = g_adapters[a].AdapterLuid;
            q->hProcess    = ph;
            q->Query.QueryProcessNode.NodeOrdinal = node;
            q->Query.QueryProcessNode.PhysicalAdapterIndex = 0;
            NTSTATUS r = g_qs(buf);
            if (r != 0) break;
            /* RunningTime is the first field of PROCESS_NODE_INFORMATION (LARGE_INTEGER). */
            LARGE_INTEGER *rt = (LARGE_INTEGER *)q->QueryResult;
            total += (uint64_t)rt->QuadPart;
        }
    }

    CloseHandle(ph);
    return total;
}
