// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/actions.hpp"

#include <windows.h>
#include <winternl.h>
#include <shellapi.h>

#include <format>
#include <memory>

namespace ring0 {
namespace {

struct HandleCloser {
    void operator()(HANDLE h) const noexcept { if (h) CloseHandle(h); }
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

std::unexpected<std::wstring> fail(std::wstring_view what, DWORD code)
{
    return std::unexpected(std::format(L"{}: {}", what, system_message(code)));
}

std::unexpected<std::wstring> fail_last(std::wstring_view what)
{
    return fail(what, GetLastError());
}

bool refused(std::uint32_t pid) noexcept { return pid == 0 || pid == 4; }

std::unexpected<std::wstring> refuse(std::uint32_t pid)
{
    return std::unexpected(std::format(L"pid {} is a system process and cannot be modified", pid));
}

std::expected<UniqueHandle, std::wstring> open(std::uint32_t pid, DWORD access)
{
    HANDLE h = OpenProcess(access, FALSE, pid);
    if (!h) return fail_last(std::format(L"cannot open pid {}", pid));
    return UniqueHandle(h);
}

using NtProcessFn = LONG(NTAPI*)(HANDLE);
using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(LONG);

struct NtEntry {
    NtProcessFn suspend = nullptr;
    NtProcessFn resume = nullptr;
    RtlNtStatusToDosErrorFn to_dos = nullptr;
};

// ntdll is mapped into every process, so the module handle never needs releasing.
const NtEntry& nt_entry()
{
    static const NtEntry e = [] {
        NtEntry r;
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
            r.suspend = reinterpret_cast<NtProcessFn>(GetProcAddress(nt, "NtSuspendProcess"));
            r.resume  = reinterpret_cast<NtProcessFn>(GetProcAddress(nt, "NtResumeProcess"));
            r.to_dos  = reinterpret_cast<RtlNtStatusToDosErrorFn>(GetProcAddress(nt, "RtlNtStatusToDosError"));
        }
        return r;
    }();
    return e;
}

ActionResult call_nt(std::uint32_t pid, NtProcessFn fn, std::wstring_view verb)
{
    if (refused(pid)) return refuse(pid);
    if (!fn) return std::unexpected(std::format(L"cannot {} pid {}: ntdll entry point missing", verb, pid));
    auto h = open(pid, PROCESS_SUSPEND_RESUME);
    if (!h) return std::unexpected(std::move(h.error()));
    const LONG status = fn(h->get());
    if (status < 0) {
        const auto& e = nt_entry();
        const DWORD code = e.to_dos ? e.to_dos(status) : static_cast<DWORD>(status);
        return fail(std::format(L"cannot {} pid {}", verb, pid), code);
    }
    return {};
}

} // namespace

ActionResult terminate_process(std::uint32_t pid, std::uint32_t exit_code)
{
    if (refused(pid)) return refuse(pid);
    auto h = open(pid, PROCESS_TERMINATE);
    if (!h) return std::unexpected(std::move(h.error()));
    if (!TerminateProcess(h->get(), exit_code)) return fail_last(std::format(L"cannot terminate pid {}", pid));
    return {};
}

ActionResult suspend_process(std::uint32_t pid)
{
    return call_nt(pid, nt_entry().suspend, L"suspend");
}

ActionResult resume_process(std::uint32_t pid)
{
    return call_nt(pid, nt_entry().resume, L"resume");
}

ActionResult set_priority(std::uint32_t pid, PriorityClass cls)
{
    if (refused(pid)) return refuse(pid);
    auto h = open(pid, PROCESS_SET_INFORMATION);
    if (!h) return std::unexpected(std::move(h.error()));
    if (!SetPriorityClass(h->get(), static_cast<DWORD>(cls)))
        return fail_last(std::format(L"cannot set priority of pid {}", pid));
    return {};
}

std::expected<PriorityClass, std::wstring> get_priority(std::uint32_t pid)
{
    if (refused(pid)) return refuse(pid);
    auto h = open(pid, PROCESS_QUERY_LIMITED_INFORMATION);
    if (!h) return std::unexpected(std::move(h.error()));
    const DWORD cls = GetPriorityClass(h->get());
    if (!cls) return fail_last(std::format(L"cannot read priority of pid {}", pid));
    return static_cast<PriorityClass>(cls);
}

ActionResult restrict_to_single_core(std::uint32_t pid)
{
    if (refused(pid)) return refuse(pid);
    auto h = open(pid, PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION);
    if (!h) return std::unexpected(std::move(h.error()));
    if (!SetProcessAffinityMask(h->get(), 1))
        return fail_last(std::format(L"cannot set affinity of pid {}", pid));
    return {};
}

ActionResult open_file_location(std::wstring_view image_path)
{
    if (image_path.empty()) return std::unexpected(std::wstring(L"image path is unknown"));
    const std::wstring args = std::format(L"/select,\"{}\"", image_path);
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL));
    // ShellExecute reports failure as a value of 32 or below, not through GetLastError alone.
    if (r <= 32) return fail_last(L"cannot open file location");
    return {};
}

} // namespace ring0
