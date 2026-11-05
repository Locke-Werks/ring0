#include "ntapi.h"

/* Going through function pointers rather than ntdll.lib stubs so we can target
   APIs that aren't in the SDK import library on older Windows SDKs, and so
   everything NT-flavored lives in one place. */

fn_NtQuerySystemInformation   nt_qsi;
fn_NtQueryInformationProcess  nt_qip;
fn_NtSuspendProcess           nt_suspend;
fn_NtResumeProcess            nt_resume;

void nt_init(void)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return;
    nt_qsi     = (fn_NtQuerySystemInformation)  GetProcAddress(nt, "NtQuerySystemInformation");
    nt_qip     = (fn_NtQueryInformationProcess) GetProcAddress(nt, "NtQueryInformationProcess");
    nt_suspend = (fn_NtSuspendProcess)          GetProcAddress(nt, "NtSuspendProcess");
    nt_resume  = (fn_NtResumeProcess)           GetProcAddress(nt, "NtResumeProcess");
}
