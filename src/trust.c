#include "trust.h"
#include "intern.h"
#include "snapshot.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <string.h>

/* Async authenticode verification. The worker thread walks the most recently
   published snapshot, finds image-path intern handles it hasn't seen, and runs
   WinVerifyTrust on each. Results land in a slot table keyed by intern handle
   (which already dedupes paths by content). */

#define TRUST_SLOTS 2048u

struct slot {
    volatile LONG handle;     /* intern id; 0 = empty */
    volatile LONG state;      /* trust_state_t */
};

static struct slot   *g_slots;
static HANDLE         g_thread;
static HANDLE         g_exit_event;
static volatile LONG  g_running;

static struct slot *trust_slot(uint32_t handle, int insert)
{
    if (!g_slots || handle == 0) return NULL;
    unsigned h = (handle * 2654435761u) & (TRUST_SLOTS - 1);
    for (unsigned i = 0; i < TRUST_SLOTS; ++i) {
        struct slot *s = &g_slots[(h + i) & (TRUST_SLOTS - 1)];
        LONG cur = s->handle;
        if (cur == (LONG)handle) return s;
        if (cur == 0) {
            if (!insert) return NULL;
            if (InterlockedCompareExchange(&s->handle, (LONG)handle, 0) == 0) return s;
            if (s->handle == (LONG)handle) return s;
        }
    }
    return NULL;
}

uint8_t trust_query(uint32_t handle)
{
    struct slot *s = trust_slot(handle, 0);
    if (!s) return TRUST_UNKNOWN;
    return (uint8_t)s->state;
}

static uint8_t verify_one(const wchar_t *path)
{
    WINTRUST_FILE_INFO fi;
    memset(&fi, 0, sizeof fi);
    fi.cbStruct       = sizeof fi;
    fi.pcwszFilePath  = path;

    WINTRUST_DATA td;
    memset(&td, 0, sizeof td);
    td.cbStruct            = sizeof td;
    td.dwUIChoice          = WTD_UI_NONE;
    td.fdwRevocationChecks = WTD_REVOKE_NONE;
    td.dwUnionChoice       = WTD_CHOICE_FILE;
    td.pFile               = &fi;
    td.dwStateAction       = WTD_STATEACTION_VERIFY;
    td.dwProvFlags         = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG r = WinVerifyTrust(NULL, &action, &td);

    td.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &action, &td);

    if (r == ERROR_SUCCESS)              return TRUST_SIGNED;
    if (r == (LONG)TRUST_E_NOSIGNATURE)  return TRUST_UNSIGNED;
    return TRUST_BAD;
}

static DWORD WINAPI worker(LPVOID p)
{
    (void)p;
    while (WaitForSingleObject(g_exit_event, 0) == WAIT_TIMEOUT) {
        const snapshot_t *snap = snapshot_current();
        int worked = 0;
        if (snap) {
            for (uint32_t i = 0; i < snap->n_processes; ++i) {
                if (WaitForSingleObject(g_exit_event, 0) != WAIT_TIMEOUT) break;
                uint32_t handle = snap->processes[i].image_path;
                if (!handle) continue;
                struct slot *s = trust_slot(handle, 1);
                if (!s || s->state != TRUST_UNKNOWN) continue;

                const wchar_t *path = intern_get(handle);
                if (!path) continue;
                uint8_t state = verify_one(path);
                InterlockedExchange(&s->state, state);
                worked = 1;
            }
        }
        /* if there was nothing new, idle longer */
        WaitForSingleObject(g_exit_event, worked ? 50 : 1000);
    }
    InterlockedExchange(&g_running, 0);
    return 0;
}

void trust_start(void)
{
    if (g_running) return;
    if (!g_slots) {
        g_slots = (struct slot *)VirtualAlloc(NULL,
            sizeof(struct slot) * TRUST_SLOTS,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_slots) return;
    }
    g_exit_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
}

void trust_stop(void)
{
    if (!g_thread) return;
    SetEvent(g_exit_event);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    CloseHandle(g_exit_event);
    g_thread = NULL;
    g_exit_event = NULL;
}
