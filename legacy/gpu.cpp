#include "gpu.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>     /* NTSTATUS */
#include <dxgi1_4.h>
#include <string.h>

/* ── D3DKMT types ────────────────────────────────────────────────────────── */

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
    D3DKMT_QS_SEGMENT          = 3,
    D3DKMT_QS_PROCESS_SEGMENT  = 4,
    D3DKMT_QS_NODE             = 5,
    D3DKMT_QS_PROCESS_NODE     = 6,
} D3DKMT_QUERYSTATISTICS_TYPE_R0;

typedef struct _D3DKMT_QUERYSTATISTICS_QUERY_NODE_R0 {
    UINT16 NodeOrdinal;
    UINT16 PhysicalAdapterIndex;
} D3DKMT_QUERYSTATISTICS_QUERY_NODE_R0;

typedef union _D3DKMT_QUERYSTATISTICS_QUERY_R0 {
    D3DKMT_QUERYSTATISTICS_QUERY_NODE_R0 QueryProcessNode;
    BYTE pad[16];
} D3DKMT_QUERYSTATISTICS_QUERY_R0;

typedef BYTE D3DKMT_QUERYSTATISTICS_RESULT_R0[1024];

typedef struct _D3DKMT_QUERYSTATISTICS_R0 {
    D3DKMT_QUERYSTATISTICS_TYPE_R0      Type;
    LUID                                AdapterLuid;
    HANDLE                              hProcess;
    D3DKMT_QUERYSTATISTICS_RESULT_R0    QueryResult;
    D3DKMT_QUERYSTATISTICS_QUERY_R0     Query;
} D3DKMT_QUERYSTATISTICS_R0;

/* D3DKMTQueryAdapterInfo for engine metadata */
#define KMTQAITYPE_NODEMETADATA 33
struct r0_queryadapterinfo {
    D3DKMT_HANDLE hAdapter;
    UINT          Type;
    void         *pPrivateDriverData;
    UINT          PrivateDriverDataSize;
};
struct r0_nodemetadata {
    UINT  NodeOrdinalAndAdapterIndex;
    UINT  EngineType;
    WCHAR FriendlyName[32];    /* DXGK_MAX_METADATA_NAME_LENGTH */
    BYTE  _tail[64];
};

/* D3DKMT_QUERYSTATISTICS_SYSTEM_MEMORY — 72 bytes */
struct r0_sys_mem {
    uint64_t BytesAllocated;
    uint64_t BytesReserved;
    uint32_t SmallAllocationBlocks;
    uint32_t LargeAllocationBlocks;
    uint64_t WriteCombinedBytesAllocated;
    uint64_t WriteCombinedBytesReserved;
    uint64_t CachedBytesAllocated;
    uint64_t CachedBytesReserved;
    uint64_t SectionBytesAllocated;
    uint64_t SectionBytesReserved;
};

/* D3DKMT_QUERYSTATISTICS_PROCESS_INFORMATION — RunningTime at offset 152 */
struct r0_process_info {
    uint32_t     NodeCount;
    uint32_t     VidPnSourceCount;
    r0_sys_mem   SystemMemory;
    r0_sys_mem   Reserved;
    uint64_t     RunningTime;
    /* more fields follow */
};

/* ── NVAPI types ─────────────────────────────────────────────────────────── */

struct NV_GPU_THERMAL_SETTINGS {
    uint32_t version;
    uint32_t count;
    struct {
        int32_t controller;
        int32_t defaultMinTemp;
        int32_t defaultMaxTemp;
        int32_t currentTemp;
        int32_t target;
    } sensor[3];
};

/* ── function pointers ───────────────────────────────────────────────────── */

typedef NTSTATUS (NTAPI *fn_enum)(D3DKMT_ENUMADAPTERS2_R0 *);
typedef NTSTATUS (NTAPI *fn_qs)  (void *);
typedef NTSTATUS (NTAPI *fn_qai) (void *);
typedef void*    (__cdecl *fn_nvapi_qi)(unsigned int);
typedef int      (__cdecl *fn_nvapi_thermal)(void *, int, NV_GPU_THERMAL_SETTINGS *);

/* ── globals ─────────────────────────────────────────────────────────────── */

static fn_enum                   g_enum;
static fn_qs                     g_qs;
static fn_qai                    g_qai;
static D3DKMT_ADAPTERINFO_R0     g_adapters[8];
static ULONG                     g_n_adapters;

/* DXGI */
static IDXGIAdapter3            *g_dxgi_adapter;
static wchar_t                   g_adapter_name[128];

/* NVAPI */
static void                     *g_nvapi_gpu;
static fn_nvapi_thermal           g_nvapi_thermal;

/* per-engine state — g_engine_node[i] maps engine index → D3DKMT node ordinal
   so gpu_sample queries the correct node even when unnamed nodes are skipped. */
static int                        g_engine_count;
static wchar_t                    g_engine_names[GPU_ENGINE_MAX][64];
static uint16_t                   g_engine_node[GPU_ENGINE_MAX];
static uint64_t                   g_engine_prev_rt[GPU_ENGINE_MAX];
static uint16_t                   g_engine_pct_val[GPU_ENGINE_MAX];
static uint64_t                   g_prev_tick;

/* ── init helpers ────────────────────────────────────────────────────────── */

static void init_dxgi(void)
{
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)))
        return;

    IDXGIAdapter1 *adapter1 = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter1) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter1->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter1->Release(); continue; }
        /* store adapter name */
        lstrcpynW(g_adapter_name, desc.Description, 127);
        g_adapter_name[127] = 0;
        adapter1->QueryInterface(__uuidof(IDXGIAdapter3), (void **)&g_dxgi_adapter);
        adapter1->Release();
        break;
    }
    factory->Release();
}

static void init_nvapi(void)
{
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi) return;
    fn_nvapi_qi qi = (fn_nvapi_qi)GetProcAddress(nvapi, "nvapi_QueryInterface");
    if (!qi) return;

    typedef int (__cdecl *fn_init)(void);
    typedef int (__cdecl *fn_enum_gpus)(void *handles[64], int *count);

    fn_init init = (fn_init)qi(0x0150E828);
    if (!init || init() != 0) return;

    fn_enum_gpus enum_gpus = (fn_enum_gpus)qi(0xE5AC921F);
    if (!enum_gpus) return;

    void *handles[64];
    int count = 0;
    if (enum_gpus(handles, &count) != 0 || count == 0) return;

    g_nvapi_gpu     = handles[0];
    g_nvapi_thermal = (fn_nvapi_thermal)qi(0xE3640A56);
}

static void init_engines(void)
{
    if (!g_qs || g_n_adapters == 0) return;

    /* get node count from the first adapter via D3DKMT_QS_ADAPTER */
    BYTE buf[1280];
    memset(buf, 0, sizeof buf);
    auto *q = (D3DKMT_QUERYSTATISTICS_R0 *)buf;
    q->Type        = D3DKMT_QS_ADAPTER;
    q->AdapterLuid = g_adapters[0].AdapterLuid;
    q->hProcess    = nullptr;
    if (g_qs(buf) != 0) return;

    uint32_t node_count = *(uint32_t *)(q->QueryResult + 4);
    if (node_count == 0) return;
    if (node_count > 16) node_count = 16;

    /* try to get engine names via D3DKMTQueryAdapterInfo NODEMETADATA */
    int got_metadata = 0;
    if (g_qai) {
        for (uint32_t n = 0; n < node_count && g_engine_count < GPU_ENGINE_MAX; ++n) {
            r0_nodemetadata md = {};
            md.NodeOrdinalAndAdapterIndex = (UINT)n;
            r0_queryadapterinfo qai = {};
            qai.hAdapter             = g_adapters[0].hAdapter;
            qai.Type                 = KMTQAITYPE_NODEMETADATA;
            qai.pPrivateDriverData   = &md;
            qai.PrivateDriverDataSize = sizeof(md);
            if (g_qai(&qai) != 0) continue;
            got_metadata = 1;
            if (md.FriendlyName[0] == 0) continue;
            lstrcpynW(g_engine_names[g_engine_count], md.FriendlyName, 63);
            g_engine_names[g_engine_count][63] = 0;
            g_engine_node[g_engine_count] = (uint16_t)n;
            ++g_engine_count;
        }
    }

    /* fallback: if metadata query failed, register all nodes with generic names */
    if (!got_metadata && g_engine_count == 0) {
        static const wchar_t *fallback[] = {
            L"3D", L"Copy", L"Video Decode", L"Video Encode",
            L"Video Processing", L"Engine 5", L"Engine 6", L"Engine 7"
        };
        for (uint32_t n = 0; n < node_count && g_engine_count < GPU_ENGINE_MAX; ++n) {
            lstrcpynW(g_engine_names[g_engine_count],
                      n < 8 ? fallback[n] : L"Engine", 63);
            g_engine_node[g_engine_count] = (uint16_t)n;
            ++g_engine_count;
        }
    }
}

/* ── public API ──────────────────────────────────────────────────────────── */

extern "C" void gpu_init(void)
{
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) return;
    g_enum = (fn_enum)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    g_qs   = (fn_qs)  GetProcAddress(gdi, "D3DKMTQueryStatistics");
    g_qai  = (fn_qai) GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    if (!g_enum || !g_qs) { g_enum = nullptr; g_qs = nullptr; return; }

    D3DKMT_ENUMADAPTERS2_R0 e = { 0, nullptr };
    g_enum(&e);
    if (e.NumAdapters == 0) e.NumAdapters = 8;
    if (e.NumAdapters > 8) e.NumAdapters = 8;
    e.pAdapters = g_adapters;
    if (g_enum(&e) == 0) g_n_adapters = e.NumAdapters;

    init_dxgi();
    init_nvapi();
    init_engines();
}

extern "C" void gpu_destroy(void)
{
    if (g_dxgi_adapter) { g_dxgi_adapter->Release(); g_dxgi_adapter = nullptr; }
    g_enum = nullptr;
    g_qs   = nullptr;
    g_qai  = nullptr;
    g_n_adapters  = 0;
    g_engine_count = 0;
    g_nvapi_gpu    = nullptr;
    g_nvapi_thermal = nullptr;
}

extern "C" uint64_t gpu_runtime_100ns(unsigned pid)
{
    if (!g_qs || g_n_adapters == 0 || pid == 0) return 0;
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) return 0;

    uint64_t total = 0;
    for (ULONG a = 0; a < g_n_adapters; ++a) {
        for (UINT16 node = 0; node < 16; ++node) {
            BYTE buf[1280];
            memset(buf, 0, sizeof buf);
            auto *q = (D3DKMT_QUERYSTATISTICS_R0 *)buf;
            q->Type        = D3DKMT_QS_PROCESS_NODE;
            q->AdapterLuid = g_adapters[a].AdapterLuid;
            q->hProcess    = ph;
            q->Query.QueryProcessNode.NodeOrdinal = node;
            q->Query.QueryProcessNode.PhysicalAdapterIndex = 0;
            NTSTATUS r = g_qs(buf);
            if (r != 0) break;
            auto *rt = (LARGE_INTEGER *)q->QueryResult;
            total += (uint64_t)rt->QuadPart;
        }
    }
    CloseHandle(ph);
    return total;
}

extern "C" void gpu_sample(uint64_t tick_ms)
{
    if (!g_qs || g_n_adapters == 0 || g_engine_count == 0) return;

    uint64_t elapsed = tick_ms - g_prev_tick;
    if (elapsed == 0) elapsed = 1;

    BYTE buf[1280];

    /* query each engine's node by its stored ordinal */
    for (int ei = 0; ei < g_engine_count; ++ei) {
        memset(buf, 0, sizeof buf);
        auto *q = (D3DKMT_QUERYSTATISTICS_R0 *)buf;
        q->Type        = D3DKMT_QS_NODE;
        q->AdapterLuid = g_adapters[0].AdapterLuid;
        q->hProcess    = nullptr;
        q->Query.QueryProcessNode.NodeOrdinal = g_engine_node[ei];
        q->Query.QueryProcessNode.PhysicalAdapterIndex = 0;
        NTSTATUS r = g_qs(buf);
        if (r != 0) { g_engine_pct_val[ei] = 0; continue; }

        /* RunningTime sits inside GlobalInformation (r0_process_info) at
           offset 152 from the start of QueryResult. */
        uint64_t rt = *(uint64_t *)(q->QueryResult + offsetof(r0_process_info, RunningTime));

        if (g_prev_tick > 0) {
            uint64_t drt = rt - g_engine_prev_rt[ei];
            uint64_t pct_x10 = drt / (elapsed * 10);
            g_engine_pct_val[ei] = pct_x10 > 1000 ? 1000 : (uint16_t)pct_x10;
        }
        g_engine_prev_rt[ei] = rt;
    }

    g_prev_tick = tick_ms;
}

extern "C" int gpu_query_memory(gpu_mem_info_t *out)
{
    if (!g_dxgi_adapter || !out) return 0;
    memset(out, 0, sizeof(*out));

    DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
    if (SUCCEEDED(g_dxgi_adapter->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
        out->dedicated_used  = local.CurrentUsage;
        out->dedicated_total = local.Budget;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO shared = {};
    if (SUCCEEDED(g_dxgi_adapter->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared))) {
        out->shared_used  = shared.CurrentUsage;
        out->shared_total = shared.Budget;
    }
    return 1;
}

extern "C" int gpu_query_temp(void)
{
    if (!g_nvapi_thermal || !g_nvapi_gpu) return -1;
    NV_GPU_THERMAL_SETTINGS ts = {};
    ts.version = sizeof(ts) | (1 << 16);
    if (g_nvapi_thermal(g_nvapi_gpu, 0, &ts) != 0) return -1;
    if (ts.count == 0) return -1;
    return ts.sensor[0].currentTemp;
}

extern "C" const wchar_t *gpu_adapter_name(void)
{
    return g_adapter_name[0] ? g_adapter_name : nullptr;
}

extern "C" int gpu_engine_count(void)   { return g_engine_count; }
extern "C" uint16_t gpu_engine_pct(int i)
{
    if (i < 0 || i >= g_engine_count) return 0;
    return g_engine_pct_val[i];
}
extern "C" const wchar_t *gpu_engine_name(int i)
{
    if (i < 0 || i >= g_engine_count) return nullptr;
    return g_engine_names[i];
}
