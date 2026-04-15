#include "etw.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <string.h>

/* NT Kernel Logger session driven through evntrace.h. Captures DiskIo read/write
   and TcpIp send/recv, folds the byte counts into a per-PID counter table. Only
   one kernel logger can exist, so we stop any orphan first. */

static const GUID SystemTraceGuid =
    { 0x9E814AAD, 0x3204, 0x11D2, { 0x9A, 0x82, 0x00, 0x60, 0x08, 0xA8, 0x69, 0x39 } };

static const GUID DiskIoGuid =
    { 0x3D6FA8D4, 0xFE05, 0x11D0, { 0x9D, 0xDA, 0x00, 0xC0, 0x4F, 0xD7, 0xBA, 0x7C } };

static const GUID TcpIpGuid =
    { 0x9A280AC0, 0xC8E0, 0x11D1, { 0x84, 0xE2, 0x00, 0xC0, 0x4F, 0xB9, 0x98, 0xA2 } };

#pragma pack(push, 1)
typedef struct {
    ULONG     DiskNumber;
    ULONG     IrpFlags;
    ULONG     TransferSize;
    ULONG     Reserved;
    LONGLONG  ByteOffset;
    ULONG_PTR FileObject;
    ULONG_PTR Irp;
    ULONGLONG HighResResponseTime;
    ULONG     IssuingThreadId;
} diskio_evt_t;

typedef struct {
    ULONG  PID;
    ULONG  size;
    ULONG  daddr;
    ULONG  saddr;
    USHORT dport;
    USHORT sport;
    ULONG  seqnum;
    ULONG  connid;
} tcpip_evt_t;
#pragma pack(pop)

#define CTR_SLOTS 2048u

struct slot {
    volatile LONG   pid;     /* 0 = empty */
    volatile LONG64 disk_read;
    volatile LONG64 disk_write;
    volatile LONG64 net_in;
    volatile LONG64 net_out;
};

static struct slot    *g_ctr;     /* VirtualAlloc'd — keeps bss small */
static TRACEHANDLE     g_session;
static TRACEHANDLE     g_consumer;
static HANDLE          g_thread;
static volatile LONG   g_running;

static struct slot *slot_for(unsigned pid, int insert)
{
    if (pid == 0 || !g_ctr) return NULL;
    unsigned h = (pid * 2654435761u) & (CTR_SLOTS - 1);
    for (unsigned i = 0; i < CTR_SLOTS; ++i) {
        struct slot *s = &g_ctr[(h + i) & (CTR_SLOTS - 1)];
        LONG cur = s->pid;
        if (cur == (LONG)pid) return s;
        if (cur == 0) {
            if (!insert) return NULL;
            if (InterlockedCompareExchange(&s->pid, (LONG)pid, 0) == 0) return s;
            if (s->pid == (LONG)pid) return s;
        }
    }
    return NULL;
}

static VOID WINAPI event_cb(PEVENT_RECORD ev)
{
    if (memcmp(&ev->EventHeader.ProviderId, &DiskIoGuid, sizeof(GUID)) == 0) {
        UCHAR op = ev->EventHeader.EventDescriptor.Opcode;
        if ((op == 10 || op == 11) && ev->UserDataLength >= sizeof(ULONG) * 3) {
            diskio_evt_t *p = (diskio_evt_t *)ev->UserData;
            struct slot *s = slot_for(ev->EventHeader.ProcessId, 1);
            if (!s) return;
            if (op == 10) InterlockedAdd64(&s->disk_read,  (LONG64)p->TransferSize);
            else          InterlockedAdd64(&s->disk_write, (LONG64)p->TransferSize);
        }
        return;
    }
    if (memcmp(&ev->EventHeader.ProviderId, &TcpIpGuid, sizeof(GUID)) == 0) {
        UCHAR op = ev->EventHeader.EventDescriptor.Opcode;
        if ((op == 10 || op == 11) && ev->UserDataLength >= sizeof(tcpip_evt_t)) {
            tcpip_evt_t *p = (tcpip_evt_t *)ev->UserData;
            struct slot *s = slot_for(p->PID, 1);
            if (!s) return;
            if (op == 10) InterlockedAdd64(&s->net_out, (LONG64)p->size);
            else          InterlockedAdd64(&s->net_in,  (LONG64)p->size);
        }
    }
}

static DWORD WINAPI consumer_thread(LPVOID p)
{
    (void)p;
    EVENT_TRACE_LOGFILEW lf;
    memset(&lf, 0, sizeof lf);
    lf.LoggerName        = (LPWSTR)KERNEL_LOGGER_NAMEW;
    lf.ProcessTraceMode  = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = event_cb;

    g_consumer = OpenTraceW(&lf);
    if (g_consumer == INVALID_PROCESSTRACE_HANDLE) {
        InterlockedExchange(&g_running, 0);
        return 0;
    }
    ProcessTrace(&g_consumer, 1, NULL, NULL);
    CloseTrace(g_consumer);
    g_consumer = 0;
    InterlockedExchange(&g_running, 0);
    return 0;
}

#define PROPS_BYTES  (sizeof(EVENT_TRACE_PROPERTIES) + 2 * sizeof(KERNEL_LOGGER_NAMEW))

static void fill_props(EVENT_TRACE_PROPERTIES *props, ULONG flags)
{
    memset(props, 0, PROPS_BYTES);
    props->Wnode.BufferSize    = PROPS_BYTES;
    props->Wnode.Guid          = SystemTraceGuid;
    props->Wnode.ClientContext = 1;                 /* QPC timestamps */
    props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    props->EnableFlags         = flags;
    props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
    props->FlushTimer          = 1;
    props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
    /* session name is written by StartTrace when LogFileNameOffset = 0 */
}

void etw_start(void)
{
    if (g_running) return;

    if (!g_ctr) {
        g_ctr = (struct slot *)VirtualAlloc(NULL, sizeof(struct slot) * CTR_SLOTS,
                                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_ctr) return;
    }

    BYTE buf[PROPS_BYTES];
    EVENT_TRACE_PROPERTIES *props = (EVENT_TRACE_PROPERTIES *)buf;

    /* try once; if another kernel session is alive, kill it then retry */
    ULONG flags = EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_NETWORK_TCPIP;
    fill_props(props, flags);
    ULONG r = StartTraceW(&g_session, KERNEL_LOGGER_NAMEW, props);
    if (r == ERROR_ALREADY_EXISTS) {
        fill_props(props, 0);
        ControlTraceW(0, KERNEL_LOGGER_NAMEW, props, EVENT_TRACE_CONTROL_STOP);
        fill_props(props, flags);
        r = StartTraceW(&g_session, KERNEL_LOGGER_NAMEW, props);
    }
    if (r != ERROR_SUCCESS) {
        g_session = 0;
        return;
    }

    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, consumer_thread, NULL, 0, NULL);
    if (!g_thread) {
        etw_stop();
    }
}

void etw_stop(void)
{
    if (g_session) {
        BYTE buf[PROPS_BYTES];
        EVENT_TRACE_PROPERTIES *props = (EVENT_TRACE_PROPERTIES *)buf;
        fill_props(props, 0);
        ControlTraceW(g_session, KERNEL_LOGGER_NAMEW, props, EVENT_TRACE_CONTROL_STOP);
        g_session = 0;
    }
    if (g_thread) {
        /* ProcessTrace returns when the session stops */
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}

void etw_counters_for(unsigned pid, etw_counters_t *out)
{
    if (!out) return;
    struct slot *s = slot_for(pid, 0);
    if (!s) {
        memset(out, 0, sizeof *out);
        return;
    }
    out->disk_read  = (uint64_t)s->disk_read;
    out->disk_write = (uint64_t)s->disk_write;
    out->net_in     = (uint64_t)s->net_in;
    out->net_out    = (uint64_t)s->net_out;
}

int etw_is_running(void) { return (int)g_running; }
