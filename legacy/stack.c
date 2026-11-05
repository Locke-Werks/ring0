#include "stack.h"
#include "render.h"
#include "intern.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <string.h>

/* One-shot stack sample for every thread in `pid` — uses StackWalk64 on the
   thread context after SuspendThread, then resolves PCs to module!symbol via
   dbghelp loaded lazily. UI thread blocks for the duration; tolerable for the
   ~10s timeline. */

#define MAX_FRAMES 256

typedef BOOL    (WINAPI *p_SymInitializeW)(HANDLE, PCWSTR, BOOL);
typedef BOOL    (WINAPI *p_SymCleanup)(HANDLE);
typedef BOOL    (WINAPI *p_SymFromAddrW)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFOW);
typedef BOOL    (WINAPI *p_SymGetModuleInfoW64)(HANDLE, DWORD64, PIMAGEHLP_MODULEW64);
typedef BOOL    (WINAPI *p_StackWalk64)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64,
                                        PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                                        PFUNCTION_TABLE_ACCESS_ROUTINE64,
                                        PGET_MODULE_BASE_ROUTINE64,
                                        PTRANSLATE_ADDRESS_ROUTINE64);
typedef PVOID   (WINAPI *p_SymFunctionTableAccess64)(HANDLE, DWORD64);
typedef DWORD64 (WINAPI *p_SymGetModuleBase64)(HANDLE, DWORD64);

static p_SymInitializeW            sym_init;
static p_SymCleanup                sym_cleanup_fn;
static p_SymFromAddrW              sym_from_addr;
static p_SymGetModuleInfoW64       sym_module_info;
static p_StackWalk64               stack_walk_fn;
static p_SymFunctionTableAccess64  sym_func_table;
static p_SymGetModuleBase64        sym_module_base;

static stack_frame_t g_frames[MAX_FRAMES];
static int           g_n;

static int dbghelp_load(void)
{
    if (sym_init) return 1;
    HMODULE m = LoadLibraryW(L"dbghelp.dll");
    if (!m) return 0;
    sym_init        = (p_SymInitializeW)GetProcAddress(m, "SymInitializeW");
    sym_cleanup_fn  = (p_SymCleanup)GetProcAddress(m, "SymCleanup");
    sym_from_addr   = (p_SymFromAddrW)GetProcAddress(m, "SymFromAddrW");
    sym_module_info = (p_SymGetModuleInfoW64)GetProcAddress(m, "SymGetModuleInfoW64");
    stack_walk_fn   = (p_StackWalk64)GetProcAddress(m, "StackWalk64");
    sym_func_table  = (p_SymFunctionTableAccess64)GetProcAddress(m, "SymFunctionTableAccess64");
    sym_module_base = (p_SymGetModuleBase64)GetProcAddress(m, "SymGetModuleBase64");
    return sym_init && stack_walk_fn && sym_func_table && sym_module_base;
}

static int frame_idx(uint64_t addr)
{
    for (int i = 0; i < g_n; ++i) if (g_frames[i].addr == addr) return i;
    if (g_n >= MAX_FRAMES) return -1;
    g_frames[g_n].addr = addr;
    g_frames[g_n].module = 0;
    g_frames[g_n].symbol = 0;
    g_frames[g_n].hits = 0;
    return g_n++;
}

static void resolve_symbol(HANDLE hp, int i)
{
    BYTE sbuf[sizeof(SYMBOL_INFOW) + 256 * sizeof(WCHAR)];
    PSYMBOL_INFOW sym = (PSYMBOL_INFOW)sbuf;
    memset(sbuf, 0, sizeof sbuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
    sym->MaxNameLen   = 256;

    wchar_t out[300];
    int     len;

    DWORD64 disp = 0;
    if (sym_from_addr && sym_from_addr(hp, g_frames[i].addr, &disp, sym) && sym->NameLen) {
        len = wsprintfW(out, L"%s+0x%X", sym->Name, (unsigned)disp);
    } else {
        len = wsprintfW(out, L"0x%016I64X", g_frames[i].addr);
    }
    g_frames[i].symbol = intern_str(out, (size_t)len);

    IMAGEHLP_MODULEW64 mi;
    memset(&mi, 0, sizeof mi);
    mi.SizeOfStruct = sizeof mi;
    if (sym_module_info && sym_module_info(hp, g_frames[i].addr, &mi)) {
        g_frames[i].module = intern_str(mi.ModuleName, lstrlenW(mi.ModuleName));
    } else {
        g_frames[i].module = intern_str(L"?", 1);
    }
}

int stack_capture(unsigned pid)
{
    g_n = 0;
    if (pid == 0 || pid == 0xFFFFFFFFu) return 0;
    if (!dbghelp_load()) return 0;

    HANDLE hp = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_SUSPEND_RESUME,
        FALSE, pid);
    if (!hp) return 0;

    sym_init(hp, NULL, TRUE);   /* TRUE = invade process for module list */

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid) continue;
                HANDLE ht = OpenThread(
                    THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                    THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!ht) continue;

                SuspendThread(ht);

                CONTEXT ctx;
                memset(&ctx, 0, sizeof ctx);
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(ht, &ctx)) {
                    STACKFRAME64 sf;
                    memset(&sf, 0, sizeof sf);
                    sf.AddrPC.Offset    = ctx.Rip;
                    sf.AddrPC.Mode      = AddrModeFlat;
                    sf.AddrFrame.Offset = ctx.Rbp;
                    sf.AddrFrame.Mode   = AddrModeFlat;
                    sf.AddrStack.Offset = ctx.Rsp;
                    sf.AddrStack.Mode   = AddrModeFlat;

                    for (int depth = 0; depth < 32; ++depth) {
                        if (!stack_walk_fn(IMAGE_FILE_MACHINE_AMD64,
                                hp, ht, &sf, &ctx,
                                NULL, sym_func_table, sym_module_base, NULL)) break;
                        if (sf.AddrPC.Offset == 0) break;
                        int idx = frame_idx(sf.AddrPC.Offset);
                        if (idx < 0) break;
                        ++g_frames[idx].hits;
                    }
                }

                ResumeThread(ht);
                CloseHandle(ht);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }

    for (int i = 0; i < g_n; ++i) resolve_symbol(hp, i);

    if (sym_cleanup_fn) sym_cleanup_fn(hp);
    CloseHandle(hp);

    /* insertion sort by hits desc */
    for (int i = 1; i < g_n; ++i) {
        stack_frame_t cur = g_frames[i];
        int j = i - 1;
        while (j >= 0 && g_frames[j].hits < cur.hits) {
            g_frames[j + 1] = g_frames[j];
            --j;
        }
        g_frames[j + 1] = cur;
    }

    render_set_stack_frames(g_frames, g_n, pid);
    return g_n;
}

void stack_clear(void)
{
    g_n = 0;
    render_set_stack_frames(NULL, 0, 0);
}
