// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/weight_stores.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <vector>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace ring0 {
namespace {

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return n && n < std::size(buf) ? std::wstring(buf, n) : std::wstring{};
}

std::filesystem::path ollama_models_dir() {
    if (auto d = env(L"OLLAMA_MODELS"); !d.empty()) return d;
    if (auto h = env(L"USERPROFILE"); !h.empty()) return std::filesystem::path(h) / L".ollama" / L"models";
    return {};
}

// Manifests are small JSON; the weights layer carries the image.model media type.
std::wstring model_digest(const std::filesystem::path& manifest) {
    std::ifstream f(manifest, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > (1u << 20)) return {};
    const auto mt = s.find("application/vnd.ollama.image.model");
    if (mt == std::string::npos) return {};
    // The digest sits in the same layer object, before or after the media type.
    const auto obj_start = s.rfind('{', mt);
    const auto obj_end = s.find('}', mt);
    if (obj_start == std::string::npos || obj_end == std::string::npos) return {};
    const auto layer = s.substr(obj_start, obj_end - obj_start);
    const auto d = layer.find("sha256:");
    if (d == std::string::npos) return {};
    std::wstring hex;
    for (std::size_t i = d + 7; i < layer.size() && std::isxdigit(static_cast<unsigned char>(layer[i])); ++i)
        hex.push_back(static_cast<wchar_t>(std::tolower(static_cast<unsigned char>(layer[i]))));
    return hex;
}

} // namespace

void WeightStores::refresh_ollama() {
    const auto now = GetTickCount64();
    if (ollama_tick_ && now - ollama_tick_ < 60'000) return;
    ollama_tick_ = now;
    ollama_.clear();

    const auto root = ollama_models_dir() / L"manifests";
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return;
    // manifests/<registry>/<namespace>/<name>/<tag>
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (it.depth() != 3 || !it->is_regular_file(ec)) continue;
        const auto& p = it->path();
        auto digest = model_digest(p);
        if (digest.empty()) continue;
        const auto rel = std::filesystem::relative(p, root, ec);
        std::vector<std::wstring> parts;
        for (const auto& c : rel) parts.push_back(c.wstring());
        if (parts.size() != 4) continue;
        std::wstring name = parts[2] + L":" + parts[3];
        if (parts[0] != L"registry.ollama.ai" || parts[1] != L"library") name = parts[1] + L"/" + name;
        ollama_.emplace(std::move(digest), std::move(name));
    }
}

std::wstring WeightStores::friendly_name(const std::wstring& path) {
    const std::filesystem::path p(path);
    const auto l = lower(path);
    const auto file = p.filename().wstring();

    if (auto b = l.find(L"sha256-"); b != std::wstring::npos && l.find(L"blobs") != std::wstring::npos) {
        refresh_ollama();
        if (auto it = ollama_.find(l.substr(b + 7)); it != ollama_.end()) return it->second;
        return L"ollama blob " + l.substr(b + 7, 12);
    }
    // Hugging Face cache: .../models--org--repo/snapshots/<rev>/file
    if (auto m = l.find(L"models--"); m != std::wstring::npos) {
        auto rest = path.substr(m + 8);
        rest = rest.substr(0, rest.find_first_of(L"\\/"));
        if (auto dd = rest.find(L"--"); dd != std::wstring::npos) rest.replace(dd, 2, L"/");
        return rest + L" (" + file + L")";
    }
    // LM Studio: .../models/<publisher>/<repo>/file
    if (l.find(L"lmstudio") != std::wstring::npos || l.find(L"lm-studio") != std::wstring::npos) {
        const auto repo = p.parent_path().filename().wstring();
        const auto pub = p.parent_path().parent_path().filename().wstring();
        if (!repo.empty() && !pub.empty()) return pub + L"/" + repo;
    }
    return file;
}

} // namespace ring0
