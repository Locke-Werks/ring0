// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/nt.hpp"

namespace ring0::nt {

namespace {

template <class Fn>
Fn resolve(HMODULE mod, const char* name) noexcept {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(mod, name)));
}

Api load() noexcept {
    Api a;
    // ntdll is mapped into every process before any user code runs, so no LoadLibrary.
    HMODULE mod = ::GetModuleHandleW(L"ntdll.dll");
    if (!mod) return a;
    a.query_system_information  = resolve<NtQuerySystemInformationFn>(mod, "NtQuerySystemInformation");
    a.query_information_process = resolve<NtQueryInformationProcessFn>(mod, "NtQueryInformationProcess");
    a.suspend_process           = resolve<NtSuspendProcessFn>(mod, "NtSuspendProcess");
    a.resume_process            = resolve<NtResumeProcessFn>(mod, "NtResumeProcess");
    return a;
}

} // namespace

const Api& api() noexcept {
    static const Api a = load();
    return a;
}

} // namespace ring0::nt
