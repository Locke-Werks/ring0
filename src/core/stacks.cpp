// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/stacks.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include <format>
#include <memory>
#include <mutex>

#pragma comment(lib, "dbghelp.lib")

namespace ring0 {
namespace {

// Matches the legacy sampler; deeper walks mostly add loader and thunk frames.
constexpr int kMaxDepth = 32;

std::mutex g_dbghelp_mutex;

struct HandleCloser {
    void operator()(HANDLE h) const noexcept { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

std::wstring system_message(DWORD code)
{
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring msg;
    if (n && buf) {
        msg.assign(buf, n);
        while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' '))
            msg.pop_back();
    }
    if (buf) LocalFree(buf);
    if (msg.empty()) msg = std::format(L"error {}", code);
    return msg;
}

std::unexpected<std::wstring> fail_last(std::wstring_view what)
{
    return std::unexpected(std::format(L"{}: {}", what, system_message(GetLastError())));
}

class SymSession {
public:
    explicit SymSession(HANDLE process) noexcept
        : process_(process), ok_(SymInitializeW(process, nullptr, TRUE) != FALSE) {}
    ~SymSession() { if (ok_) SymCleanup(process_); }
    SymSession(const SymSession&) = delete;
    SymSession& operator=(const SymSession&) = delete;
    explicit operator bool() const noexcept { return ok_; }
private:
    HANDLE process_;
    bool ok_;
};

// Resumes on every exit path, so an early return cannot leave a target thread frozen.
class SuspendGuard {
public:
    explicit SuspendGuard(HANDLE thread) noexcept
        : thread_(thread), ok_(SuspendThread(thread) != static_cast<DWORD>(-1)) {}
    ~SuspendGuard() { if (ok_) ResumeThread(thread_); }
    SuspendGuard(const SuspendGuard&) = delete;
    SuspendGuard& operator=(const SuspendGuard&) = delete;
    explicit operator bool() const noexcept { return ok_; }
private:
    HANDLE thread_;
    bool ok_;
};

std::vector<std::uint64_t> walk(HANDLE process, HANDLE thread)
{
    std::vector<std::uint64_t> pcs;
    SuspendGuard suspended(thread);
    if (!suspended) return pcs;

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) return pcs;

    STACKFRAME64 sf{};
#if defined(_M_X64)
    constexpr DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    sf.AddrPC.Offset    = ctx.Rip;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
    constexpr DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    sf.AddrPC.Offset    = ctx.Pc;
    sf.AddrFrame.Offset = ctx.Fp;
    sf.AddrStack.Offset = ctx.Sp;
#else
#error "unsupported architecture"
#endif
    sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;

    for (int depth = 0; depth < kMaxDepth; ++depth) {
        if (!StackWalk64(machine, process, thread, &sf, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (sf.AddrPC.Offset == 0) break;
        pcs.push_back(sf.AddrPC.Offset);
    }
    return pcs;
}

StackFrame resolve(HANDLE process, std::uint64_t addr)
{
    StackFrame f;
    f.address = addr;

    IMAGEHLP_MODULEW64 mi{};
    mi.SizeOfStruct = sizeof mi;
    if (SymGetModuleInfoW64(process, addr, &mi)) {
        f.module = mi.ModuleName;
        f.offset = addr - mi.BaseOfImage;
    } else {
        f.module = L"?";
    }

    alignas(SYMBOL_INFOW) BYTE buf[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)]{};
    auto* sym = reinterpret_cast<SYMBOL_INFOW*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
    sym->MaxNameLen   = MAX_SYM_NAME;
    DWORD64 disp = 0;
    if (SymFromAddrW(process, addr, &disp, sym) && sym->NameLen) {
        f.symbol.assign(sym->Name, sym->NameLen);
        f.offset = disp;
    }
    return f;
}

} // namespace

std::expected<std::vector<ThreadStack>, std::wstring> capture_stacks(std::uint32_t pid)
{
    if (pid == 0 || pid == 4) return std::unexpected(std::format(L"pid {} has no user-mode stacks", pid));

    UniqueHandle process(OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_SUSPEND_RESUME, FALSE, pid));
    if (!process) return fail_last(std::format(L"cannot open pid {}", pid));

    std::lock_guard lock(g_dbghelp_mutex);
    SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymSession session(process.get());
    if (!session) return fail_last(std::format(L"cannot load symbols for pid {}", pid));

    UniqueHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (snap.get() == INVALID_HANDLE_VALUE) return fail_last(L"cannot enumerate threads");

    // Suspending the calling thread would never return, so it is skipped when the
    // target is this process.
    const DWORD self_tid = pid == GetCurrentProcessId() ? GetCurrentThreadId() : 0;

    struct Raw { std::uint32_t tid; std::vector<std::uint64_t> pcs; };
    std::vector<Raw> raw;

    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL more = Thread32First(snap.get(), &te); more; more = Thread32Next(snap.get(), &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self_tid) continue;
        UniqueHandle thread(OpenThread(
            THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID));
        if (!thread) continue;
        raw.push_back({te.th32ThreadID, walk(process.get(), thread.get())});
    }

    std::vector<ThreadStack> out;
    out.reserve(raw.size());
    for (auto& r : raw) {
        ThreadStack ts;
        ts.tid = r.tid;
        ts.frames.reserve(r.pcs.size());
        for (std::uint64_t pc : r.pcs) ts.frames.push_back(resolve(process.get(), pc));
        out.push_back(std::move(ts));
    }
    return out;
}

} // namespace ring0
