// SPDX-License-Identifier: GPL-3.0-or-later
// winsock2 must precede windows.h, which nt.hpp pulls in.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "core/inspect.hpp"
#include "core/nt.hpp"

#include <psapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace ring0 {
namespace {

// Not in winternl.h; returns a single PS_PROTECTION byte and works with a limited handle.
constexpr ULONG kProcessProtectionInformation = 61;

void add_error(ProcessDetail& d, std::wstring_view msg) {
    if (!d.error.empty()) d.error += L"; ";
    d.error += msg;
}

std::wstring lower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

// Module lists and the shell disagree on prefixes, separators and case; comparisons
// only work after all three are folded away.
std::wstring normalize(std::wstring_view p) {
    if (p.starts_with(L"\\\\?\\")) p.remove_prefix(4);
    std::wstring s = lower(p);
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

std::wstring long_path(std::wstring p) {
    // GetTempPathW can hand back an 8.3 form that never prefix-matches a module path.
    std::array<wchar_t, 32768> buf;
    DWORD n = ::GetLongPathNameW(p.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    if (n > 0 && n < buf.size()) return std::wstring(buf.data(), n);
    return p;
}

std::wstring_view dir_of(std::wstring_view p) {
    auto pos = p.find_last_of(L"\\/");
    return pos == std::wstring_view::npos ? std::wstring_view{} : p.substr(0, pos);
}

std::wstring_view file_of(std::wstring_view p) {
    auto pos = p.find_last_of(L"\\/");
    return pos == std::wstring_view::npos ? p : p.substr(pos + 1);
}

bool under(std::wstring_view path, std::wstring_view root) {
    if (root.empty() || path.size() < root.size()) return false;
    if (path.substr(0, root.size()) != root) return false;
    return path.size() == root.size() || path[root.size()] == L'\\';
}

const std::vector<std::wstring>& writable_roots() {
    static const std::vector<std::wstring> roots = [] {
        std::vector<std::wstring> r;
        for (const KNOWNFOLDERID* id : {&FOLDERID_LocalAppData, &FOLDERID_RoamingAppData,
                                        &FOLDERID_Downloads, &FOLDERID_Public}) {
            PWSTR p = nullptr;
            if (SUCCEEDED(::SHGetKnownFolderPath(*id, 0, nullptr, &p)) && p)
                r.push_back(normalize(long_path(p)));
            ::CoTaskMemFree(p);
        }
        std::array<wchar_t, MAX_PATH + 1> tmp;
        DWORD n = ::GetTempPathW(static_cast<DWORD>(tmp.size()), tmp.data());
        if (n > 0 && n < tmp.size()) r.push_back(normalize(long_path(std::wstring(tmp.data(), n))));
        std::erase_if(r, [](const std::wstring& s) { return s.size() <= 3; });
        return r;
    }();
    return roots;
}

// Returns the matching root so a signal can name it.
std::optional<std::wstring> writable_root_of(std::wstring_view path) {
    std::wstring dir = normalize(dir_of(path));
    for (const auto& root : writable_roots())
        if (under(dir, root)) return root;
    return std::nullopt;
}

const std::wstring& system32() {
    static const std::wstring dir = [] {
        std::array<wchar_t, MAX_PATH + 1> buf;
        UINT n = ::GetSystemDirectoryW(buf.data(), static_cast<UINT>(buf.size()));
        return n > 0 && n < buf.size() ? normalize(std::wstring_view(buf.data(), n)) : std::wstring{};
    }();
    return dir;
}

// verify() is synchronous and a cold catalog lookup costs hundreds of milliseconds,
// so results outlive any one inspection. Files can change underneath, but a process
// inspector re-verifying the same system DLLs every call is the worse trade.
TrustInfo cached_verify(const std::wstring& path) {
    static std::mutex mutex;
    static std::unordered_map<std::wstring, TrustInfo> cache;
    std::wstring key = normalize(path);
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    // Unlocked while verifying so concurrent inspections do not serialize on one slow file.
    TrustInfo t = verify(path);
    std::lock_guard lock(mutex);
    return cache.try_emplace(std::move(key), std::move(t)).first->second;
}

std::wstring query_image_path(HANDLE ph) {
    std::array<wchar_t, 32768> buf;
    DWORD sz = static_cast<DWORD>(buf.size());
    if (::QueryFullProcessImageNameW(ph, 0, buf.data(), &sz) && sz > 0)
        return std::wstring(buf.data(), sz);
    return {};
}

std::wstring query_command_line(HANDLE ph) {
    auto qip = nt::api().query_information_process;
    if (!qip) return {};
    // UNICODE_STRING header followed by the text; 8-byte elements keep the header aligned.
    std::vector<std::uint64_t> buf(512);
    for (int tries = 0; tries < 4; ++tries) {
        ULONG bytes = static_cast<ULONG>(buf.size() * sizeof(std::uint64_t));
        ULONG ret = 0;
        NTSTATUS st = qip(ph, nt::kProcessCommandLineInformation, buf.data(), bytes, &ret);
        if (nt::nt_success(st)) {
            if (ret < sizeof(UNICODE_STRING)) return {};
            const auto* us = reinterpret_cast<const UNICODE_STRING*>(buf.data());
            if (!us->Buffer || us->Length == 0) return {};
            return std::wstring(us->Buffer, us->Length / sizeof(WCHAR));
        }
        if (st != nt::kStatusInfoLengthMismatch && st != nt::kStatusBufferTooSmall &&
            st != nt::kStatusBufferOverflow)
            return {};
        if (ret <= bytes) return {};
        buf.resize(ret / sizeof(std::uint64_t) + 1);
    }
    return {};
}

// nullopt when the query itself is unavailable, so the caller can fall back to guessing.
std::optional<bool> query_protected(HANDLE ph) {
    auto qip = nt::api().query_information_process;
    if (!qip) return std::nullopt;
    BYTE prot = 0;
    ULONG ret = 0;
    if (!nt::nt_success(qip(ph, kProcessProtectionInformation, &prot, sizeof(prot), &ret)))
        return std::nullopt;
    return (prot & 0x7) != 0;   // low three bits are the protection type; zero means none
}

std::uint64_t query_create_time(HANDLE ph) {
    FILETIME c{}, e{}, k{}, u{};
    if (!::GetProcessTimes(ph, &c, &e, &k, &u)) return 0;
    return (static_cast<std::uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}

std::wstring query_arch(HANDLE ph) {
    USHORT proc = IMAGE_FILE_MACHINE_UNKNOWN, native = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!::IsWow64Process2(ph, &proc, &native)) return {};
    // UNKNOWN for the process machine means it is not under WOW64, so it is native.
    USHORT m = proc == IMAGE_FILE_MACHINE_UNKNOWN ? native : proc;
    switch (m) {
    case IMAGE_FILE_MACHINE_AMD64: return L"x64";
    case IMAGE_FILE_MACHINE_I386:  return L"x86";
    case IMAGE_FILE_MACHINE_ARM64: return L"arm64";
    case IMAGE_FILE_MACHINE_ARMNT: return L"arm";
    default:                       return {};
    }
}

std::vector<BYTE> token_info(HANDLE tok, TOKEN_INFORMATION_CLASS cls) {
    DWORD need = 0;
    ::GetTokenInformation(tok, cls, nullptr, 0, &need);
    if (need == 0) return {};
    std::vector<BYTE> buf(need);
    if (!::GetTokenInformation(tok, cls, buf.data(), need, &need)) return {};
    return buf;
}

std::wstring sid_to_name(PSID sid) {
    std::array<wchar_t, 256> name{}, domain{};
    DWORD nn = static_cast<DWORD>(name.size()), dn = static_cast<DWORD>(domain.size());
    SID_NAME_USE use{};
    if (!::LookupAccountSidW(nullptr, sid, name.data(), &nn, domain.data(), &dn, &use)) return {};
    std::wstring out(domain.data(), dn);
    if (!out.empty()) out += L'\\';
    out.append(name.data(), nn);
    return out;
}

bool is_service_sid(PSID sid) {
    return ::IsWellKnownSid(sid, WinLocalSystemSid) || ::IsWellKnownSid(sid, WinLocalServiceSid) ||
           ::IsWellKnownSid(sid, WinNetworkServiceSid);
}

// Returns whether the account is a built-in service identity, for the elevation signal.
bool read_token(HANDLE ph, ProcessDetail& d) {
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(ph, TOKEN_QUERY, &raw)) {
        add_error(d, L"token not readable (access denied)");
        return false;
    }
    nt::unique_handle tok(raw);
    bool service = false;

    if (auto b = token_info(tok.get(), TokenUser); !b.empty()) {
        PSID sid = reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid;
        d.user = sid_to_name(sid);
        service = is_service_sid(sid);
    }
    if (auto b = token_info(tok.get(), TokenIntegrityLevel); !b.empty()) {
        PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(b.data())->Label.Sid;
        DWORD count = *::GetSidSubAuthorityCount(sid);
        if (count > 0) {
            DWORD rid = *::GetSidSubAuthority(sid, count - 1);
            if (rid >= SECURITY_MANDATORY_PROTECTED_PROCESS_RID) d.integrity = L"protected";
            else if (rid >= SECURITY_MANDATORY_SYSTEM_RID)       d.integrity = L"system";
            else if (rid >= SECURITY_MANDATORY_HIGH_RID)         d.integrity = L"high";
            else if (rid >= SECURITY_MANDATORY_MEDIUM_RID)       d.integrity = L"medium";
            else                                                 d.integrity = L"low";
        }
    }
    TOKEN_ELEVATION el{};
    DWORD len = 0;
    if (::GetTokenInformation(tok.get(), TokenElevation, &el, sizeof(el), &len))
        d.elevated = el.TokenIsElevated != 0;
    return service;
}

struct ProcEntry {
    std::uint32_t ppid = 0;
    std::wstring exe;
};

std::unordered_map<std::uint32_t, ProcEntry> process_table() {
    std::unordered_map<std::uint32_t, ProcEntry> t;
    nt::unique_handle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return t;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = ::Process32FirstW(snap.get(), &pe); ok; ok = ::Process32NextW(snap.get(), &pe))
        t[pe.th32ProcessID] = {pe.th32ParentProcessID, pe.szExeFile};
    return t;
}

std::uint64_t create_time_of(std::uint32_t pid) {
    auto h = nt::open_process(PROCESS_QUERY_LIMITED_INFORMATION, pid);
    return h ? query_create_time(h.get()) : 0;
}

void walk_ancestry(ProcessDetail& d, const std::unordered_map<std::uint32_t, ProcEntry>& table) {
    std::uint32_t child = d.pid;
    std::uint64_t child_time = d.create_time;
    for (int depth = 0; depth < 32; ++depth) {
        auto it = table.find(child);
        if (it == table.end()) break;
        std::uint32_t parent = it->second.ppid;
        if (parent == 0 || parent == child) break;
        if (!table.contains(parent)) break;
        // A pid outliving its process gets recycled; a "parent" born after the child is
        // a stranger. Unknown times count as unproven, so the walk stops there too.
        std::uint64_t pt = create_time_of(parent);
        if (pt == 0 || child_time == 0 || pt >= child_time) break;
        d.ancestry.push_back(parent);
        child = parent;
        child_time = pt;
    }
}

bool enum_modules_psapi(HANDLE ph, std::vector<ModuleInfo>& out) {
    std::vector<HMODULE> mods(512);
    DWORD need = 0;
    for (int tries = 0; tries < 4; ++tries) {
        DWORD bytes = static_cast<DWORD>(mods.size() * sizeof(HMODULE));
        if (!::EnumProcessModulesEx(ph, mods.data(), bytes, &need, LIST_MODULES_ALL)) return false;
        if (need <= bytes) break;
        mods.resize(need / sizeof(HMODULE) + 16);
    }
    mods.resize(std::min<std::size_t>(mods.size(), need / sizeof(HMODULE)));
    std::array<wchar_t, 32768> buf;
    for (HMODULE m : mods) {
        DWORD n = ::GetModuleFileNameExW(ph, m, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) continue;
        ModuleInfo mi;
        mi.path.assign(buf.data(), n);
        MODULEINFO info{};
        if (::GetModuleInformation(ph, m, &info, sizeof(info))) {
            mi.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
            mi.size = info.SizeOfImage;
        } else {
            mi.base = reinterpret_cast<std::uintptr_t>(m);
        }
        out.push_back(std::move(mi));
    }
    return !out.empty();
}

bool enum_modules_toolhelp(std::uint32_t pid, std::vector<ModuleInfo>& out) {
    nt::unique_handle snap;
    // ERROR_BAD_LENGTH means the target was mid-load; the documented answer is to retry.
    for (int tries = 0; tries < 4 && !snap; ++tries) {
        snap.reset(::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (!snap && ::GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (!snap) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    for (BOOL ok = ::Module32FirstW(snap.get(), &me); ok; ok = ::Module32NextW(snap.get(), &me)) {
        ModuleInfo mi;
        mi.path = me.szExePath;
        mi.base = reinterpret_cast<std::uintptr_t>(me.modBaseAddr);
        mi.size = me.modBaseSize;
        out.push_back(std::move(mi));
    }
    return !out.empty();
}

const wchar_t* tcp_state(DWORD s) {
    switch (s) {
    case MIB_TCP_STATE_CLOSED:     return L"closed";
    case MIB_TCP_STATE_LISTEN:     return L"listen";
    case MIB_TCP_STATE_SYN_SENT:   return L"syn_sent";
    case MIB_TCP_STATE_SYN_RCVD:   return L"syn_received";
    case MIB_TCP_STATE_ESTAB:      return L"established";
    case MIB_TCP_STATE_FIN_WAIT1:  return L"fin_wait1";
    case MIB_TCP_STATE_FIN_WAIT2:  return L"fin_wait2";
    case MIB_TCP_STATE_CLOSE_WAIT: return L"close_wait";
    case MIB_TCP_STATE_CLOSING:    return L"closing";
    case MIB_TCP_STATE_LAST_ACK:   return L"last_ack";
    case MIB_TCP_STATE_TIME_WAIT:  return L"time_wait";
    case MIB_TCP_STATE_DELETE_TCB: return L"delete_tcb";
    default:                       return L"unknown";
    }
}

std::wstring endpoint(int family, const void* addr, DWORD port) {
    std::array<wchar_t, 64> buf{};
    if (!::InetNtopW(family, addr, buf.data(), buf.size())) return {};
    auto p = ::ntohs(static_cast<u_short>(port));
    return family == AF_INET6 ? std::format(L"[{}]:{}", buf.data(), p)
                              : std::format(L"{}:{}", buf.data(), p);
}

// The tables are sized by a first call that fails; between the two calls sockets can
// open, so the second call is retried a few times rather than trusted once.
template <class Query>
std::vector<std::uint64_t> fetch_table(Query q) {
    std::vector<std::uint64_t> buf;
    DWORD size = 0;
    for (int tries = 0; tries < 4; ++tries) {
        DWORD rc = q(buf.empty() ? nullptr : buf.data(), &size);
        if (rc == NO_ERROR) return buf;
        if (rc != ERROR_INSUFFICIENT_BUFFER) return {};
        buf.assign(size / sizeof(std::uint64_t) + 1, 0);
    }
    return {};
}

void read_connections(ProcessDetail& d) {
    const DWORD pid = d.pid;
    bool any_failed = false;

    auto tcp4 = fetch_table([](void* b, DWORD* s) {
        return ::GetExtendedTcpTable(b, s, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    });
    if (tcp4.empty()) any_failed = true;
    else {
        const auto* t = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(tcp4.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            const auto& r = t->table[i];
            if (r.dwOwningPid != pid) continue;
            d.connections.push_back({L"tcp", endpoint(AF_INET, &r.dwLocalAddr, r.dwLocalPort),
                                     endpoint(AF_INET, &r.dwRemoteAddr, r.dwRemotePort),
                                     tcp_state(r.dwState)});
        }
    }

    auto tcp6 = fetch_table([](void* b, DWORD* s) {
        return ::GetExtendedTcpTable(b, s, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
    });
    if (tcp6.empty()) any_failed = true;
    else {
        const auto* t = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(tcp6.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            const auto& r = t->table[i];
            if (r.dwOwningPid != pid) continue;
            d.connections.push_back({L"tcp6", endpoint(AF_INET6, r.ucLocalAddr, r.dwLocalPort),
                                     endpoint(AF_INET6, r.ucRemoteAddr, r.dwRemotePort),
                                     tcp_state(r.dwState)});
        }
    }

    auto udp4 = fetch_table([](void* b, DWORD* s) {
        return ::GetExtendedUdpTable(b, s, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    });
    if (udp4.empty()) any_failed = true;
    else {
        const auto* t = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(udp4.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            const auto& r = t->table[i];
            if (r.dwOwningPid != pid) continue;
            d.connections.push_back({L"udp", endpoint(AF_INET, &r.dwLocalAddr, r.dwLocalPort), {}, {}});
        }
    }

    auto udp6 = fetch_table([](void* b, DWORD* s) {
        return ::GetExtendedUdpTable(b, s, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0);
    });
    if (udp6.empty()) any_failed = true;
    else {
        const auto* t = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(udp6.data());
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            const auto& r = t->table[i];
            if (r.dwOwningPid != pid) continue;
            d.connections.push_back({L"udp6", endpoint(AF_INET6, r.ucLocalAddr, r.dwLocalPort), {}, {}});
        }
    }

    if (any_failed) add_error(d, L"some connection tables could not be read");
}

void derive_signals(ProcessDetail& d, bool service_account, bool is_protected,
                    const std::unordered_map<std::uint32_t, ProcEntry>& table) {
    auto& s = d.findings;

    if (d.image_trust.state == TrustState::Unsigned) s.push_back(L"image is unsigned");
    else if (d.image_trust.state == TrustState::Invalid) s.push_back(L"image signature is invalid");

    if (!d.image_path.empty())
        if (auto root = writable_root_of(d.image_path))
            s.push_back(std::format(L"image runs from a user-writable folder ({})", *root));

    std::vector<std::wstring_view> unsigned_names;
    std::size_t unsigned_count = 0, writable_count = 0;
    for (const auto& m : d.modules) {
        bool bad = m.trust.state == TrustState::Unsigned || m.trust.state == TrustState::Invalid;
        if (bad) {
            ++unsigned_count;
            if (unsigned_names.size() < 2) unsigned_names.push_back(file_of(m.path));
        }
        // Signed per-user installs (OneDrive, Teams shell extensions) live in AppData on
        // stock machines; only an unsigned module from such a folder is worth a line.
        if (m.user_writable_dir && m.trust.state != TrustState::Signed) ++writable_count;
    }
    if (unsigned_count > 0) {
        std::wstring names;
        for (auto n : unsigned_names) {
            if (!names.empty()) names += L", ";
            names += n;
        }
        s.push_back(std::format(L"{} unsigned module{} loaded, including {}", unsigned_count,
                                unsigned_count == 1 ? L"" : L"s", names));
    }
    if (writable_count > 0)
        s.push_back(std::format(L"{} unsigned module{} loaded from user-writable folders",
                                writable_count, writable_count == 1 ? L"" : L"s"));

    // Parent and location checks only run on a known path and a verified parent, so a
    // missing answer can never become an accusation.
    if (!d.image_path.empty()) {
        std::wstring path = normalize(d.image_path);
        std::wstring name(file_of(path));
        std::wstring dir(dir_of(path));
        bool in_sys32 = !system32().empty() && dir == system32();

        static constexpr std::array<std::wstring_view, 6> pinned = {
            L"services.exe", L"wininit.exe", L"csrss.exe", L"smss.exe", L"lsass.exe", L"svchost.exe"};
        if (std::ranges::find(pinned, name) != pinned.end() && !in_sys32)
            s.push_back(std::format(L"{} is running from {} instead of System32", name, dir));

        std::wstring parent_name;
        if (!d.ancestry.empty())
            if (auto it = table.find(d.ancestry.front()); it != table.end())
                parent_name = lower(it->second.exe);

        if (in_sys32 && !parent_name.empty()) {
            if (name == L"svchost.exe" && parent_name != L"services.exe")
                s.push_back(std::format(L"svchost.exe was started by {} instead of services.exe", parent_name));
            if (name == L"lsass.exe" && parent_name != L"wininit.exe")
                s.push_back(std::format(L"lsass.exe was started by {} instead of wininit.exe", parent_name));
        }
    }

    // SYSTEM and the service accounts are elevated by definition; naming them would
    // fire on every service on the box.
    if (d.elevated && !service_account && !d.user.empty())
        s.push_back(std::format(L"elevated, running as {}", d.user));

    if (is_protected) s.push_back(L"process is protected; detail is limited");
}

void inspect_into(ProcessDetail& d) {
    auto table = process_table();
    if (auto it = table.find(d.pid); it != table.end()) d.ppid = it->second.ppid;
    else if (table.empty()) add_error(d, L"process snapshot failed");
    else add_error(d, L"process not found");

    auto limited = nt::open_process(PROCESS_QUERY_LIMITED_INFORMATION, d.pid);
    if (!limited) {
        DWORD err = ::GetLastError();
        add_error(d, err == ERROR_ACCESS_DENIED ? L"process not openable (access denied)"
                                                : L"process not openable");
        // The socket tables are keyed by pid and need no handle, so they still answer.
        read_connections(d);
        return;
    }

    d.image_path = query_image_path(limited.get());
    d.create_time = query_create_time(limited.get());
    d.arch = query_arch(limited.get());
    d.command_line = query_command_line(limited.get());
    if (d.command_line.empty()) add_error(d, L"command line not readable");
    bool service_account = read_token(limited.get(), d);

    walk_ancestry(d, table);

    auto full = nt::open_process(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, d.pid);
    bool denied_full = !full && ::GetLastError() == ERROR_ACCESS_DENIED;

    // The protection byte is authoritative. Without it, a denial after a successful
    // limited open is the classic signature of a protected process, though an
    // unelevated caller looking at a SYSTEM service sees the same thing.
    bool is_protected = false;
    if (auto p = query_protected(limited.get())) is_protected = *p;
    else is_protected = denied_full;
    if (is_protected) {
        d.integrity = L"protected";
        add_error(d, L"protected process: modules and memory are not readable");
    }

    if (!d.image_path.empty()) d.image_trust = cached_verify(d.image_path);
    else add_error(d, L"image path not readable");

    bool got = full && enum_modules_psapi(full.get(), d.modules);
    if (!got) {
        d.modules.clear();
        got = enum_modules_toolhelp(d.pid, d.modules);
    }
    if (!got && !is_protected)
        add_error(d, denied_full ? L"modules not readable (access denied)" : L"modules not readable");

    std::unordered_map<std::wstring, TrustInfo> seen;
    for (auto& m : d.modules) {
        std::wstring key = normalize(m.path);
        auto it = seen.find(key);
        if (it == seen.end()) it = seen.emplace(key, cached_verify(m.path)).first;
        m.trust = it->second;
        m.user_writable_dir = writable_root_of(m.path).has_value();
    }

    read_connections(d);
    derive_signals(d, service_account, is_protected, table);
}

} // namespace

ProcessDetail inspect_process(std::uint32_t pid) {
    ProcessDetail d;
    d.pid = pid;
    try {
        inspect_into(d);
    } catch (...) {
        try { add_error(d, L"inspection aborted by an internal error"); } catch (...) {}
    }
    return d;
}

} // namespace ring0
