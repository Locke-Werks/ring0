// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/elevate.hpp"

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace ring0::elevate {
namespace {

constexpr DWORD kSudoInline = 3;

std::optional<DWORD> sudo_mode()
{
    DWORD value = 0, size = sizeof value;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Sudo",
                     L"Enabled", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

const wchar_t* describe(std::optional<DWORD> mode)
{
    if (!mode) return L"not configured";
    switch (*mode) {
    case 0: return L"disabled";
    case 1: return L"ForceNewWindow";
    case 2: return L"DisableInput";
    case kSudoInline: return L"Inline";
    default: return L"unknown";
    }
}

bool has_flag(std::wstring_view cmdline, std::wstring_view flag)
{
    // Whole-token match so a path containing the text cannot trip it.
    for (std::size_t at = cmdline.find(flag); at != std::wstring_view::npos; at = cmdline.find(flag, at + 1)) {
        const bool start = at == 0 || cmdline[at - 1] == L' ' || cmdline[at - 1] == L'"';
        const auto end = at + flag.size();
        const bool stop = end == cmdline.size() || cmdline[end] == L' ' || cmdline[end] == L'"';
        if (start && stop) return true;
    }
    return false;
}

// Everything after the program token, exactly as we were given it, so
// arguments are forwarded with their original quoting.
std::wstring_view arguments(std::wstring_view cmdline)
{
    std::size_t i = 0;
    if (!cmdline.empty() && cmdline[0] == L'"') {
        i = cmdline.find(L'"', 1);
        i = i == std::wstring_view::npos ? cmdline.size() : i + 1;
    } else {
        while (i < cmdline.size() && cmdline[i] != L' ' && cmdline[i] != L'\t') ++i;
    }
    while (i < cmdline.size() && (cmdline[i] == L' ' || cmdline[i] == L'\t')) ++i;
    return cmdline.substr(i);
}

void refuse(const std::wstring& text)
{
    MessageBoxW(nullptr, text.c_str(), L"ring0", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

} // namespace

bool is_elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD got = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &got) != 0;
    CloseHandle(token);
    return ok && e.TokenIsElevated != 0;
}

Gate gate(int& exit_code)
{
    exit_code = 0;
    const std::wstring_view cmdline = GetCommandLineW();

    // The child never gets a second opinion, whatever its token says.
    if (has_flag(cmdline, kElevatedFlag)) return Gate::Run;
    if (is_elevated()) return Gate::Run;

    wchar_t allow[8] = {};
    if (GetEnvironmentVariableW(kAllowUnelevated, allow, 8) && std::wstring_view(allow) == L"1")
        return Gate::Run;

    const auto mode = sudo_mode();
    if (mode != kSudoInline) {
        refuse(std::wstring(L"ring0 needs administrator rights and gets them through sudo for Windows "
                            L"in Inline mode. sudo is currently: ") + describe(mode) +
               L".\n\nFix it from an administrator terminal:\n    sudo config --enable normal");
        exit_code = 1;
        return Gate::Exit;
    }

    wchar_t exe[32768];
    const DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (n == 0 || n >= std::size(exe)) { exit_code = 1; return Gate::Exit; }

    std::wstring line = L"sudo.exe \"";
    line += exe;
    line += L"\"";
    if (const auto args = arguments(cmdline); !args.empty()) {
        line += L' ';
        line += args;
    }
    line += L' ';
    line += kElevatedFlag;

    // sudo is a console program; without CREATE_NO_WINDOW a launch from
    // Explorer flashes an empty console behind the UAC prompt.
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        const DWORD err = GetLastError();
        refuse(err == ERROR_FILE_NOT_FOUND
                   ? L"sudo.exe was not found. ring0 needs Windows 11 24H2 or later with sudo enabled."
                   : L"ring0 could not start sudo to elevate (error " + std::to_wstring(err) + L").");
        exit_code = 1;
        return Gate::Exit;
    }
    CloseHandle(pi.hThread);

    // Wait so whoever launched us sees the real exit code. This process holds no
    // window, no Qt and no threads; it is a few hundred KB until the child exits.
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    exit_code = int(code);
    return Gate::Exit;
}

} // namespace ring0::elevate
