// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/server_probe.hpp"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ring0 {
namespace {

constexpr int kTimeoutMs = 300;
constexpr DWORD kPeriodMs = 2000;
constexpr std::size_t kMaxBody = 1u << 20;

struct Internet {
    HINTERNET h = nullptr;
    explicit Internet(HINTERNET v) : h(v) {}
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    ~Internet() { if (h) WinHttpCloseHandle(h); }
};

std::optional<std::string> http_get(HINTERNET session, std::uint16_t port, const wchar_t* path,
                                    const std::wstring& api_key) {
    Internet conn(WinHttpConnect(session, L"127.0.0.1", port, 0));
    if (!conn.h) return std::nullopt;
    Internet req(WinHttpOpenRequest(conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!req.h) return std::nullopt;
    // A redirect could carry the key somewhere else.
    DWORD no_redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &no_redirect, sizeof no_redirect);
    std::wstring headers;
    if (!api_key.empty()) headers = L"Authorization: Bearer " + api_key + L"\r\n";
    if (!WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return std::nullopt;
    if (!WinHttpReceiveResponse(req.h, nullptr)) return std::nullopt;
    DWORD status = 0, len = sizeof status;
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX) || status != 200)
        return std::nullopt;
    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
        const auto at = body.size();
        body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, body.data() + at, avail, &got)) return std::nullopt;
        body.resize(at + got);
        if (body.size() > kMaxBody) return std::nullopt;
    }
    return body;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The responses come from a fixed set of servers with known shapes; pulling string
// fields by key is enough and keeps a JSON library out of the core.
std::vector<std::string> string_fields(std::string_view body, std::string_view key) {
    std::vector<std::string> out;
    const std::string needle = "\"" + std::string(key) + "\"";
    for (std::size_t at = body.find(needle); at != std::string_view::npos; at = body.find(needle, at + 1)) {
        auto colon = body.find(':', at + needle.size());
        if (colon == std::string_view::npos) break;
        auto q = body.find('"', colon);
        if (q == std::string_view::npos) break;
        std::string v;
        std::size_t e = q + 1;
        for (; e < body.size() && body[e] != '"'; ++e) {
            if (body[e] != '\\' || e + 1 >= body.size()) { v.push_back(body[e]); continue; }
            const char c = body[++e];
            v.push_back(c == 'n' ? '\n' : c == 't' ? '\t' : c);   // \uXXXX is not used by these servers
        }
        if (e >= body.size()) break;
        out.push_back(std::move(v));
        at = e;
    }
    return out;
}

std::uint64_t sum_number_fields(std::string_view body, std::string_view key) {
    std::uint64_t sum = 0;
    const std::string needle = "\"" + std::string(key) + "\":";
    for (std::size_t at = body.find(needle); at != std::string_view::npos; at = body.find(needle, at + 1)) {
        std::size_t i = at + needle.size();
        std::uint64_t v = 0;
        while (i < body.size() && body[i] >= '0' && body[i] <= '9') v = v * 10 + static_cast<std::uint64_t>(body[i++] - '0');
        sum += v;
    }
    return sum;
}

std::map<std::uint32_t, std::vector<std::uint16_t>> exclusive_listen_ports() {
    std::map<std::uint32_t, std::vector<std::uint16_t>> by_pid;
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
        return by_pid;
    std::vector<std::byte> buf(size);
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        return by_pid;
    std::map<std::uint16_t, std::vector<std::uint32_t>> owners;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& r = table->table[i];
        // Only sockets reachable on loopback: bound to any or to 127.0.0.1.
        if (r.dwLocalAddr != 0 && r.dwLocalAddr != htonl(INADDR_LOOPBACK)) continue;
        const auto port = ntohs(static_cast<u_short>(r.dwLocalPort));
        auto& o = owners[port];
        if (std::find(o.begin(), o.end(), r.dwOwningPid) == o.end()) o.push_back(r.dwOwningPid);
    }
    for (const auto& [port, pids] : owners)
        if (pids.size() == 1 && by_pid[pids[0]].size() < 4) by_pid[pids[0]].push_back(port);
    return by_pid;
}

bool starts_with(const std::wstring& s, std::wstring_view p) { return s.rfind(p, 0) == 0; }

// Decoded-token counts from the previous pass, so generation speed can be read while
// a request is running; the metrics gauge only moves once a request finishes.
struct DecodeMark { std::uint64_t decoded = 0; std::uint64_t tick = 0; };

ProbeResult probe(HINTERNET session, const ProbeTarget& t, const std::vector<std::uint16_t>& ports,
                  std::unordered_map<std::uint32_t, DecodeMark>& marks) {
    ProbeResult r;
    for (const auto port : ports) {
        if (starts_with(t.image, L"ollama")) {
            if (auto b = http_get(session, port, L"/api/ps", t.api_key)) {
                for (auto& n : string_fields(*b, "name")) r.loaded.push_back(widen(n));
                return r;
            }
        } else if (starts_with(t.image, L"llama-server")) {
            bool any = false;
            if (auto b = http_get(session, port, L"/metrics", t.api_key)) {
                any = true;
                constexpr std::string_view key = "llamacpp:predicted_tokens_seconds ";
                if (auto at = b->find(key); at != std::string::npos) {
                    const float avg = std::strtof(b->c_str() + at + key.size(), nullptr);
                    if (avg > 0) r.tokens_per_s = avg;
                }
            }
            if (auto b = http_get(session, port, L"/slots", t.api_key)) {
                any = true;
                int busy = 0;
                for (std::size_t at = b->find("\"is_processing\":true"); at != std::string::npos;
                     at = b->find("\"is_processing\":true", at + 1))
                    ++busy;
                r.busy_slots = busy;
                const auto decoded = sum_number_fields(*b, "n_decoded");
                const auto now = GetTickCount64();
                auto& m = marks[t.pid];
                // A count that went down means a new request replaced the old one; skip that pass.
                if (busy && m.tick && decoded > m.decoded && now > m.tick)
                    r.tokens_per_s = static_cast<float>(double(decoded - m.decoded) * 1000.0 / double(now - m.tick));
                m = {decoded, now};
            }
            if (auto b = http_get(session, port, L"/v1/models", t.api_key)) {
                any = true;
                for (auto& n : string_fields(*b, "id")) r.loaded.push_back(widen(n));
            }
            if (any) return r;
        } else if (starts_with(t.image, L"lm studio") || starts_with(t.image, L"lms")) {
            if (auto b = http_get(session, port, L"/api/v0/models", t.api_key)) {
                // Each entry carries its own state; keep the ids whose state is loaded.
                const auto ids = string_fields(*b, "id");
                const auto states = string_fields(*b, "state");
                for (std::size_t i = 0; i < ids.size() && i < states.size(); ++i)
                    if (states[i] == "loaded") r.loaded.push_back(widen(ids[i]));
                return r;
            }
        } else {
            if (auto b = http_get(session, port, L"/v1/models", t.api_key)) {
                for (auto& n : string_fields(*b, "id")) r.loaded.push_back(widen(n));
                return r;
            }
        }
    }
    return r;
}

} // namespace

bool ServerProbe::wants_probe(const std::wstring& img) {
    return starts_with(img, L"ollama") || starts_with(img, L"llama-server") ||
           starts_with(img, L"lm studio") || starts_with(img, L"lms") || img.find(L"vllm") != std::wstring::npos;
}

ServerProbe::ServerProbe()
    : wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      thread_([this](std::stop_token st) { run(st); })
{
}

ServerProbe::~ServerProbe() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
    if (wake_) CloseHandle(wake_);
}

void ServerProbe::set_targets(std::vector<ProbeTarget> targets) {
    std::scoped_lock lk(mu_);
    targets_ = std::move(targets);
}

std::unordered_map<std::uint32_t, ProbeResult> ServerProbe::results() const {
    std::scoped_lock lk(mu_);
    return results_;
}

void ServerProbe::run(std::stop_token st) {
    SetThreadDescription(GetCurrentThread(), L"ring0 server probe");
    std::stop_callback cb(st, [this] { if (wake_) SetEvent(wake_); });
    Internet session(WinHttpOpen(L"ring0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return;
    WinHttpSetTimeouts(session.h, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    std::unordered_map<std::uint32_t, DecodeMark> marks;
    while (!st.stop_requested()) {
        std::vector<ProbeTarget> targets;
        {
            std::scoped_lock lk(mu_);
            targets = targets_;
        }
        std::unordered_map<std::uint32_t, ProbeResult> fresh;
        if (!targets.empty()) {
            const auto ports = exclusive_listen_ports();
            for (const auto& t : targets) {
                if (st.stop_requested()) break;
                auto it = ports.find(t.pid);
                if (it == ports.end()) continue;
                fresh.emplace(t.pid, probe(session.h, t, it->second, marks));
            }
        }
        std::erase_if(marks, [&](const auto& kv) { return !fresh.contains(kv.first); });
        {
            std::scoped_lock lk(mu_);
            results_ = std::move(fresh);
        }
        WaitForSingleObject(wake_, kPeriodMs);
    }
}

} // namespace ring0
